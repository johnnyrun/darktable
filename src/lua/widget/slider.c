
/*
   This file is part of darktable,
   copyright (c) 2015 Jeremy Rosen

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
#include "lua/widget/common.h"
#include "bauhaus/bauhaus.h"
#include "lua/types.h"
#include "gui/gtk.h"

static void slider_init(lua_State *L);
static dt_lua_widget_type_t slider_type = {
  .name = "slider",
  .gui_init = slider_init,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};


static void slider_init(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,-1);
  dt_bauhaus_slider_from_widget(DT_BAUHAUS_WIDGET(slider->widget),NULL, 0.0, 1.0, 0.1, 0.5, 3,0);
}


static int label_member(lua_State *L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    char tmp[256];
    luaA_to(L,char_256,&tmp,3);
    dt_bauhaus_widget_set_label(slider->widget,NULL,tmp);
    return 0;
  }
  lua_pushstring(L,dt_bauhaus_widget_get_label(slider->widget));
  return 1;
}

static int hard_min_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_hard_min(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_hard_min(slider->widget));
  return 1;
}

static int hard_max_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_hard_max(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_hard_max(slider->widget));
  return 1;
}

static int soft_min_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_soft_min(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_soft_min(slider->widget));
  return 1;
}

static int soft_max_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_soft_max(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_soft_max(slider->widget));
  return 1;
}


static int value_member(lua_State *L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_soft(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get(slider->widget));
  return 1;
}

int dt_lua_init_widget_slider(lua_State* L)
{
  dt_lua_init_widget_type(L,&slider_type,lua_slider,DT_BAUHAUS_WIDGET_TYPE);

  lua_pushcfunction(L,hard_min_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_slider, "hard_min");
  lua_pushcfunction(L,hard_max_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_slider, "hard_max");
  lua_pushcfunction(L,soft_min_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_slider, "soft_min");
  lua_pushcfunction(L,soft_max_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_slider, "soft_max");
  lua_pushcfunction(L,value_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_slider, "value");
  lua_pushcfunction(L,label_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_slider, "label");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
