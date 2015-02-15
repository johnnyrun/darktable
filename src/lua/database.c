/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#include "lua/database.h"
#include "lua/events.h"
#include "lua/image.h"
#include "lua/film.h"
#include "lua/types.h"
#include "common/debug.h"
#include "common/darktable.h"
#include "common/grealpath.h"
#include "common/image.h"
#include "common/film.h"
#include "control/control.h"
#include <errno.h>

/***********************************************************************
  Creating the images global variable
 **********************************************************************/

int dt_lua_duplicate_image(lua_State *L)
{
  int imgid;
  luaA_to(L, dt_lua_image_t, &imgid, -1);
  imgid = dt_image_duplicate(imgid);
  luaA_push(L, dt_lua_image_t, &imgid);
  return 1;
}

int dt_lua_delete_image(lua_State *L)
{
  int imgid;
  luaA_to(L, dt_lua_image_t, &imgid, -1);
  dt_image_remove(imgid);
  return 0;
}

int dt_lua_move_image(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  dt_lua_film_t filmid = -1;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    luaA_to(L, dt_lua_film_t, &filmid, 2);
  }
  else
  {
    luaA_to(L, dt_lua_film_t, &filmid, 1);
    luaA_to(L, dt_lua_image_t, &imgid, 2);
  }
  dt_image_move(imgid, filmid);
  return 0;
}

int dt_lua_copy_image(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  dt_lua_film_t filmid = -1;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    luaA_to(L, dt_lua_film_t, &filmid, 2);
  }
  else
  {
    luaA_to(L, dt_lua_film_t, &filmid, 1);
    luaA_to(L, dt_lua_image_t, &imgid, 2);
  }
  dt_lua_image_t new_image = dt_image_copy(imgid, filmid);
  luaA_push(L, dt_lua_image_t, &new_image);
  return 1;
}


static int import_images(lua_State *L)
{
  char *full_name = g_realpath(luaL_checkstring(L, -1));
  int result;

  if(!full_name || !g_file_test(full_name, G_FILE_TEST_EXISTS))
  {
    g_free(full_name);
    return luaL_error(L, "no such file or directory");
  }
  else if(g_file_test(full_name, G_FILE_TEST_IS_DIR))
  {
    result = dt_film_import(full_name);
    if(result == 0)
    {
      g_free(full_name);
      return luaL_error(L, "error while importing");
    }
    luaA_push(L, dt_lua_film_t, &result);
  }
  else
  {
    dt_film_t new_film;
    dt_film_init(&new_film);
    char *dirname = g_path_get_dirname(full_name);
    char *expanded_path = dt_util_fix_path(dirname);
    g_free(dirname);
    char *final_path = g_realpath(expanded_path);
    g_free(expanded_path);
    if(!final_path)
    {
      g_free(full_name);
      return luaL_error(L, "Error while importing : %s\n", strerror(errno));
    }
    result = dt_film_new(&new_film, final_path);
    free(final_path);
    if(result == 0)
    {
      if(dt_film_is_empty(new_film.id)) dt_film_remove(new_film.id);
      dt_film_cleanup(&new_film);
      g_free(full_name);
      return luaL_error(L, "error while importing");
    }

    result = dt_image_import(new_film.id, full_name, TRUE);
    if(dt_film_is_empty(new_film.id)) dt_film_remove(new_film.id);
    dt_film_cleanup(&new_film);
    if(result == 0)
    {
      g_free(full_name);
      return luaL_error(L, "error while importing");
    }
    luaA_push(L, dt_lua_image_t, &result);
  }
  g_free(full_name);
  return 1;
}

static int database_len(lua_State *L)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(*) from images ", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    lua_pushnumber(L, sqlite3_column_int(stmt, 0));
  else
    lua_pushnumber(L, 0);
  sqlite3_finalize(stmt);
  return 1;
}

static int number_member(lua_State *L)
{
  int index = luaL_checkinteger(L, -1);
  if(index < 1)
  {
    return luaL_error(L, "incorrect index in database");
  }
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  snprintf(query, sizeof(query), "select images.id from images order by images.id limit 1 offset %d",
           index - 1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_image_t, &imgid);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L, "incorrect index in database");
  }
  return 1;
}

static void on_film_imported(gpointer instance, uint32_t id, gpointer user_data)
{
  dt_lua_lock();
  luaA_push(darktable.lua_state.state, dt_lua_film_t, &id);
  dt_lua_event_trigger(darktable.lua_state.state, "post-import-film", 1);
  dt_lua_unlock();
}

static void on_image_imported(gpointer instance, uint32_t id, gpointer user_data)
{
  dt_lua_do_chunk_async(dt_lua_event_trigger_wrapper,
      LUA_ASYNC_TYPENAME,"const char*","post-import-image",
      LUA_ASYNC_TYPENAME,"dt_lua_image_t",GINT_TO_POINTER(id),
      LUA_ASYNC_DONE);
}
int dt_lua_init_database(lua_State *L)
{

  /* database type */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "image_database", NULL);
  lua_setfield(L, -2, "database");
  lua_pop(L, 1);

  lua_pushcfunction(L, database_len);
  lua_pushcfunction(L, number_member);
  dt_lua_type_register_number_const_type(L, type_id);
  lua_pushcfunction(L, dt_lua_duplicate_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "duplicate");
  lua_pushcfunction(L, dt_lua_delete_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "delete");
  lua_pushcfunction(L, import_images);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "import");
  lua_pushcfunction(L, dt_lua_move_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "move_image");
  lua_pushcfunction(L, dt_lua_copy_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "copy_image");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "post-import-film");
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, G_CALLBACK(on_film_imported),
                            NULL);

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "post-import-image");
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_IMPORT, G_CALLBACK(on_image_imported), NULL);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
