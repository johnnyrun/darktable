/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI. (based on code by johannes hanika)

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "common/opencl.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdint.h>
#include <xmmintrin.h>

DT_MODULE_INTROSPECTION(1, dt_iop_rawprepare_params_t)

typedef struct dt_iop_rawprepare_params_t
{
  int32_t x, y, width, height; // crop, now unused, for future expansion
  uint16_t raw_black_level_separate[4];
  uint16_t raw_white_point;
} dt_iop_rawprepare_params_t;

typedef struct dt_iop_rawprepare_gui_data_t
{
  GtkWidget *box_raw;
  // TODO: GUI for cropping.
  GtkWidget *black_level_separate[4];
  GtkWidget *white_point;
  GtkWidget *label_non_raw;
} dt_iop_rawprepare_gui_data_t;

typedef struct dt_iop_rawprepare_data_t
{
  int32_t x, y, width, height; // crop, now unused, for future expansion
  float sub[4];
  float div[4];
} dt_iop_rawprepare_data_t;

typedef struct dt_iop_rawprepare_global_data_t
{
  int kernel_rawprepare_1f;
  int kernel_rawprepare_4f;
} dt_iop_rawprepare_global_data_t;

const char *name()
{
  return C_("modulename", "raw black/white point");
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE;
}

int groups()
{
  return IOP_GROUP_BASIC;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  for(int i = 0; i < 4; i++)
  {
    gchar *label = g_strdup_printf(_("black level %i"), i);
    dt_accel_register_slider_iop(self, FALSE, NC_("accel", label));
    g_free(label);
  }

  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "white point"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;

  for(int i = 0; i < 4; i++)
  {
    gchar *label = g_strdup_printf(_("black level %i"), i);
    dt_accel_connect_slider_iop(self, label, g->black_level_separate[i]);
    g_free(label);
  }

  dt_accel_connect_slider_iop(self, "white point", GTK_WIDGET(g->white_point));
}

void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_in,
                     const dt_iop_roi_t *const roi_out, dt_develop_tiling_t *tiling)
{
  float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);

  tiling->factor = 1.0f + ioratio; // in + out, no temp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 2; // Bayer pattern
  tiling->yalign = 2; // Bayer pattern
  return;
}

int output_bpp(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW))
    return sizeof(float);
  else
    return 4 * sizeof(float);
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  const float scale = piece->buf_in.scale / piece->iscale;

  const float x = (float)d->x * scale, y = (float)d->y * scale;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= x;
    points[i + 1] -= y;
  }

  return 1;
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  const float scale = piece->buf_in.scale / piece->iscale;

  const float x = (float)d->x * scale, y = (float)d->y * scale;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += x;
    points[i + 1] += y;
  }

  return 1;
}

// we're not scaling here (bayer input), so just crop borders
void modify_roi_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  roi_out->x = roi_out->y = 0;

  int32_t x = d->x + d->width, y = d->y + d->height;

  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe))
  {
    const float scale = roi_in->scale / piece->iscale;
    roi_out->width -= roundf((float)x * scale);
    roi_out->height -= roundf((float)y * scale);
  }
  else
  {
    roi_out->width -= x;
    roi_out->height -= y;
  }
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  int32_t x = d->x + d->width, y = d->y + d->height;

  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe))
  {
    const float scale = roi_in->scale / piece->iscale;
    roi_in->width += roundf((float)x * scale);
    roi_in->height += roundf((float)y * scale);
  }
  else
  {
    roi_in->width += x;
    roi_in->height += y;
  }
}

static int BL(const dt_iop_roi_t *const roi_out, const dt_iop_rawprepare_data_t *const d, const int row,
              const int col)
{
  return ((((row + roi_out->y + d->y) & 1) << 1) + ((col + roi_out->x + d->x) & 1));
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawprepare_data_t *const d = (dt_iop_rawprepare_data_t *)piece->data;

  // fprintf(stderr, "roi in %d %d %d %d\n", roi_in->x, roi_in->y, roi_in->width, roi_in->height);
  // fprintf(stderr, "roi out %d %d %d %d\n", roi_out->x, roi_out->y, roi_out->width, roi_out->height);

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  { // raw mosaic
    const int cx = d->x, cy = d->y;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(ovoid)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const uint16_t *in = ((uint16_t *)ivoid) + ((size_t)roi_in->width * (j + cy) + cx);
      float *out = ((float *)ovoid) + (size_t)roi_out->width * j;

      int i = 0;

      // FIXME: figure alignment!  !!! replace with for !!!
      while((!dt_is_aligned(in, 16) || !dt_is_aligned(out, 16)) && (i < roi_out->width))
      {
        const int id = BL(roi_out, d, j, i);
        *out = MAX(0.0f, ((float)(*in)) - d->sub[id]) / d->div[id];
        i++;
        in++;
        out++;
      }

      const __m128 sub = _mm_set_ps(d->sub[BL(roi_out, d, j, i + 3)], d->sub[BL(roi_out, d, j, i + 2)],
                                    d->sub[BL(roi_out, d, j, i + 1)], d->sub[BL(roi_out, d, j, i)]);

      const __m128 div = _mm_set_ps(d->div[BL(roi_out, d, j, i + 3)], d->div[BL(roi_out, d, j, i + 2)],
                                    d->div[BL(roi_out, d, j, i + 1)], d->div[BL(roi_out, d, j, i)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (8 - 1); i += 8, in += 8)
      {
        const __m128i input = _mm_load_si128((__m128i *)in);

        __m128i ilo = _mm_unpacklo_epi16(input, _mm_set1_epi16(0));
        __m128i ihi = _mm_unpackhi_epi16(input, _mm_set1_epi16(0));

        __m128 flo = _mm_cvtepi32_ps(ilo);
        __m128 fhi = _mm_cvtepi32_ps(ihi);

        flo = _mm_div_ps(_mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(flo, sub)), div);
        fhi = _mm_div_ps(_mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(fhi, sub)), div);

        _mm_stream_ps(out, flo);
        out += 4;
        _mm_stream_ps(out, fhi);
        out += 4;
      }

      // process the rest
      for(; i < roi_out->width; i++, in++, out++)
      {
        const int id = BL(roi_out, d, j, i);
        *out = MAX(0.0f, ((float)(*in)) - d->sub[id]) / d->div[id];
      }
    }
  }
  else
  { // pre-downsampled buffer that needs black/white scaling

    const float scale = roi_in->scale / piece->iscale;
    const int csx = d->x * scale, csy = d->y * scale;

    const __m128 sub = _mm_load_ps(d->sub), div = _mm_load_ps(d->div);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(ovoid)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)4 * (roi_in->width * (j + csy) + csx);
      float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;

      // process aligned pixels with SSE
      for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);

        const __m128 scaled = _mm_div_ps(_mm_sub_ps(input, sub), div);

        _mm_stream_ps(out, _mm_max_ps(_mm_setzero_ps(), scaled));
      }
    }
  }
  _mm_sfence();
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;
  dt_iop_rawprepare_global_data_t *gd = (dt_iop_rawprepare_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  cl_mem dev_sub = NULL;
  cl_mem dev_div = NULL;
  cl_int err = -999;

  int kernel = -1;
  int cx = -1;
  int cy = -1;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  {
    kernel = gd->kernel_rawprepare_1f;
    cx = d->x;
    cy = d->y;
  }
  else
  {
    kernel = gd->kernel_rawprepare_4f;
    const float scale = roi_in->scale / piece->iscale;
    cx = d->x * scale;
    cy = d->y * scale;
  }

  dev_sub = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->sub);
  if(dev_sub == NULL) goto error;

  dev_div = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->div);
  if(dev_div == NULL) goto error;

  const int width = roi_out->width;
  const int height = roi_out->height;

  size_t sizes[] = { ROUNDUPWD(roi_in->width), ROUNDUPHT(roi_in->height), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(width));
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(height));
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&cx);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&cy);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), (void *)&dev_sub);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), (void *)&dev_div);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(uint32_t), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(uint32_t), (void *)&roi_out->y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_sub);
  dt_opencl_release_mem_object(dev_div);

  return TRUE;

error:
  if(dev_sub != NULL) dt_opencl_release_mem_object(dev_sub);
  if(dev_div != NULL) dt_opencl_release_mem_object(dev_div);
  dt_print(DT_DEBUG_OPENCL, "[opencl_rawprepare] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void commit_params(dt_iop_module_t *self, const dt_iop_params_t *const params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_rawprepare_params_t *const p = (dt_iop_rawprepare_params_t *)params;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  d->x = p->x;
  d->y = p->y;
  d->width = p->width;
  d->height = p->height;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  {
    const float white = (float)p->raw_white_point;

    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = (float)p->raw_black_level_separate[i];
      d->div[i] = (white - d->sub[i]);
    }
  }
  else
  {
    const float white = (float)p->raw_white_point / (float)UINT16_MAX;
    float black = 0;
    for(int i = 0; i < 4; i++)
    {
      black += p->raw_black_level_separate[i] / (float)UINT16_MAX;
    }
    black /= 4.0f;

    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = black;
      d->div[i] = (white - black);
    }
  }

  if(!dt_image_is_raw(&piece->pipe->image) || piece->pipe->image.bpp == sizeof(float)) piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rawprepare_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_rawprepare_params_t tmp = { 0 };

  // we might be called from presets update infrastructure => there is no image
  if(!self->dev) goto end;

  const dt_image_t *const image = &(self->dev->image_storage);

  tmp = (dt_iop_rawprepare_params_t){.x = image->crop_x,
                                     .y = image->crop_y,
                                     .width = image->crop_width,
                                     .height = image->crop_height,
                                     .raw_black_level_separate[0] = image->raw_black_level_separate[0],
                                     .raw_black_level_separate[1] = image->raw_black_level_separate[1],
                                     .raw_black_level_separate[2] = image->raw_black_level_separate[2],
                                     .raw_black_level_separate[3] = image->raw_black_level_separate[3],
                                     .raw_white_point = image->raw_white_point };

  self->default_enabled = dt_image_is_raw(image) && image->bpp != sizeof(float);

end:
  memcpy(self->params, &tmp, sizeof(dt_iop_rawprepare_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_rawprepare_params_t));
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  self->data = malloc(sizeof(dt_iop_rawprepare_global_data_t));

  dt_iop_rawprepare_global_data_t *gd = self->data;
  gd->kernel_rawprepare_1f = dt_opencl_create_kernel(program, "rawprepare_1f");
  gd->kernel_rawprepare_4f = dt_opencl_create_kernel(program, "rawprepare_4f");
}

void init(dt_iop_module_t *self)
{
  const dt_image_t *const image = &(self->dev->image_storage);

  self->params = calloc(1, sizeof(dt_iop_rawprepare_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_rawprepare_params_t));
  self->hide_enable_button = 1;
  self->default_enabled = dt_image_is_raw(image) && image->bpp != sizeof(float);
  self->priority = 10; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_rawprepare_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_rawprepare_global_data_t *gd = (dt_iop_rawprepare_global_data_t *)self->data;
  dt_opencl_free_kernel(gd->kernel_rawprepare_4f);
  dt_opencl_free_kernel(gd->kernel_rawprepare_1f);
  free(self->data);
  self->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  for(int i = 0; i < 4; i++)
  {
    dt_bauhaus_slider_set_soft(g->black_level_separate[i], p->raw_black_level_separate[i]);
    dt_bauhaus_slider_set_default(g->black_level_separate[i], p->raw_black_level_separate[i]);
  }

  dt_bauhaus_slider_set_soft(g->white_point, p->raw_white_point);
  dt_bauhaus_slider_set_default(g->white_point, p->raw_white_point);

  if(self->default_enabled)
  {
    gtk_widget_show(g->box_raw);
    gtk_widget_hide(g->label_non_raw);
  }
  else
  {
    gtk_widget_hide(g->box_raw);
    gtk_widget_show(g->label_non_raw);
  }
}

static void callback(GtkWidget *widget, gpointer *user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  for(int i = 0; i < 4; i++)
    p->raw_black_level_separate[i] = dt_bauhaus_slider_get(g->black_level_separate[i]);
  p->raw_white_point = dt_bauhaus_slider_get(g->white_point);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rawprepare_gui_data_t));

  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->box_raw = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  for(int i = 0; i < 4; i++)
  {
    gchar *label = g_strdup_printf(_("black level %i"), i);

    g->black_level_separate[i]
        = dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, p->raw_black_level_separate[i], 0);
    dt_bauhaus_widget_set_label(g->black_level_separate[i], NULL, label);
    g_object_set(G_OBJECT(g->black_level_separate[i]), "tooltip-text", label, (char *)NULL);
    gtk_box_pack_start(GTK_BOX(g->box_raw), g->black_level_separate[i], FALSE, FALSE, 0);
    dt_bauhaus_slider_enable_soft_boundaries(g->black_level_separate[i], 0, UINT16_MAX);
    g_signal_connect(G_OBJECT(g->black_level_separate[i]), "value-changed", G_CALLBACK(callback), self);

    g_free(label);
  }

  g->white_point = dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, p->raw_white_point, 0);
  dt_bauhaus_widget_set_label(g->white_point, NULL, _("white point"));
  g_object_set(G_OBJECT(g->white_point), "tooltip-text", _("white point"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->white_point, FALSE, FALSE, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->white_point, 0, UINT16_MAX);
  g_signal_connect(G_OBJECT(g->white_point), "value-changed", G_CALLBACK(callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->box_raw, FALSE, FALSE, 0);

  g->label_non_raw
      = gtk_label_new(_("raw black/white point correction\nonly works for the sensors that need it."));
  gtk_widget_set_halign(g->label_non_raw, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), g->label_non_raw, FALSE, FALSE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
