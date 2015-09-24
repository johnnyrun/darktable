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
#include "lua/lua.h"
#include "lua/call.h"
#include "control/control.h"
#include <glib.h>
#include <sys/select.h>


typedef enum
{
  WAIT_MS,
  FILE_READABLE,
  RUN_COMMAND

} yield_type;

static int protected_to_yield(lua_State *L)
{
  yield_type type;
  luaA_to(L, yield_type, &type, 1);
  lua_pushnumber(L, type);
  return 1;
}

static int protected_to_int(lua_State *L)
{
  int result = luaL_checkinteger(L, 1);
  lua_pushnumber(L, result);
  return 1;
}

static int protected_to_userdata(lua_State *L)
{
  const char *type = lua_tostring(L, 2);
  lua_pop(L, 1);
  luaL_checkudata(L, 1, type);
  return 1;
}

static int protected_to_string(lua_State *L)
{
  luaL_checkstring(L, 1);
  // we return our first argument unchanged
  return 1;
}

int dt_lua_do_chunk(lua_State *L, int nargs, int nresults)
{
  lua_State *new_thread = lua_newthread(L);
  darktable.lua_state.pending_threads++;
  lua_insert(L, -(nargs + 2));
  lua_xmove(L, new_thread, nargs + 1);
  int thread_result = lua_resume(new_thread, L, nargs);
  do
  {
    switch(thread_result)
    {
      case LUA_OK:
        if(nresults != LUA_MULTRET)
        {
          lua_settop(new_thread, nresults);
        }
        int result = lua_gettop(new_thread);
        lua_pop(L, 1); // remove the temporary thread from the main thread
        lua_xmove(new_thread, L, result);
        darktable.lua_state.pending_threads--;
        return LUA_OK;
      case LUA_YIELD:
      {
        /*
           This code will force a thread to exit at yield
           instead of waiting for the thread to stop by itself

           This is commented out for the time being, 
           
        if(darktable.lua_state.ending) {
          lua_pop(L,1);
          if(nresults != LUA_MULTRET) for(int i = 0 ; i < nresults ; i++) lua_pushnil(L);
          darktable.lua_state.pending_threads--;
          return LUA_OK;
        }
        */
        if(lua_gettop(new_thread) == 0)
        {
          lua_pushstring(new_thread, "no parameter passed to yield");
          thread_result = LUA_ERRSYNTAX;
          goto error;
        }
        yield_type type;
        lua_pushcfunction(new_thread, protected_to_yield);
        lua_pushvalue(new_thread, 1);
        thread_result = lua_pcall(new_thread, 1, 1, 0);
        if(thread_result != LUA_OK)
        {
          goto error;
        }
        type = lua_tointeger(new_thread, -1);
        lua_pop(new_thread, 1);
        switch(type)
        {
          case WAIT_MS:
          {
            lua_pushcfunction(new_thread, protected_to_int);
            lua_pushvalue(new_thread, 2);
            thread_result = lua_pcall(new_thread, 1, 1, 0);
            if(thread_result != LUA_OK)
            {
              goto error;
            }
            int wait_time = lua_tointeger(new_thread, -1);
            lua_pop(new_thread, 3);
            dt_lua_unlock();
            g_usleep(wait_time * 1000);
            dt_lua_lock();
            thread_result = lua_resume(new_thread, L, 0);
            break;
          }
          case FILE_READABLE:
          {
            lua_pushcfunction(new_thread, protected_to_userdata);
            lua_pushvalue(new_thread, 2);
            lua_pushstring(new_thread, LUA_FILEHANDLE);
            thread_result = lua_pcall(new_thread, 2, 1, 0);
            if(thread_result != LUA_OK)
            {
              goto error;
            }
            luaL_Stream *stream = lua_touserdata(new_thread, -1);
            lua_pop(new_thread, 1);
            int myfileno = fileno(stream->f);
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(myfileno, &fdset);
            dt_lua_unlock();
            select(myfileno + 1, &fdset, NULL, NULL, 0);
            dt_lua_lock();
            thread_result = lua_resume(new_thread, L, 0);
            break;
          }
          case RUN_COMMAND:
          {
            lua_pushcfunction(new_thread, protected_to_string);
            lua_pushvalue(new_thread, 2);
            thread_result = lua_pcall(new_thread, 1, 1, 0);
            if(thread_result != LUA_OK)
            {
              goto error;
            }
            const char *command = lua_tostring(new_thread, -1);
            lua_pop(new_thread, 3);
            dt_lua_unlock();
            int result = system(command);
            dt_lua_lock();
            lua_pushinteger(new_thread, result);
            thread_result = lua_resume(new_thread, L, 1);
            break;
          }
          default:
            lua_pushstring(new_thread, "program error, shouldn't happen");
            thread_result = LUA_ERRRUN;
            goto error;
        }
        break;
      }
      default:
        goto error;
    }
  } while(true);
error:
{
  const char *error_msg = lua_tostring(new_thread, -1);
  luaL_traceback(L, new_thread, error_msg, 0);
  lua_remove(L, -2); // remove the new thread from L
  darktable.lua_state.pending_threads--;
  return thread_result;
}
}

int dt_lua_dostring(lua_State *L, const char *command, int nargs, int nresults)
{
  int load_result = luaL_loadstring(L, command);
  if(load_result != LUA_OK)
  {
    const char *error_msg = lua_tostring(L, -1);
    luaL_traceback(L, L, error_msg, 0);
    lua_remove(L, -2);
    return load_result;
  }
  lua_insert(L, -(nargs + 1));
  return dt_lua_do_chunk(L, nargs, nresults);
}

int dt_lua_dofile_silent(lua_State *L, const char *filename, int nargs, int nresults)
{
  if(luaL_loadfile(L, filename))
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return -1;
  }
  lua_insert(L, -(nargs + 1));
  return dt_lua_do_chunk_silent(L, nargs, nresults);
}

int dt_lua_dostring_silent(lua_State *L, const char *command, int nargs, int nresults)
{
  int load_result = luaL_loadstring(L, command);
  if(load_result != LUA_OK)
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return -1;
  }
  lua_insert(L, -(nargs + 1));
  return dt_lua_do_chunk_silent(L, nargs, nresults);
}

int dt_lua_do_chunk_silent(lua_State *L, int nargs, int nresults)
{
  int orig_top = lua_gettop(L);
  int thread_result = dt_lua_do_chunk(L, nargs, nresults);
  if(thread_result == LUA_OK)
  {
    return lua_gettop(L) - (orig_top -nargs -1);
  }

  if(darktable.unmuted & DT_DEBUG_LUA)
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR : %s", lua_tostring(L, -1));
  }
  lua_pop(L, 1); // remove the error message
  if(nresults != LUA_MULTRET)
  {
    for(int i = 0; i < nresults; i++)
    {
      lua_pushnil(L);
    }
    return nresults;
  }
  return 0;
}

int dt_lua_do_chunk_raise(lua_State *L, int nargs, int nresults)
{
  int orig_top = lua_gettop(L);
  int thread_result = dt_lua_do_chunk(L, nargs, nresults);
  if(thread_result == LUA_OK)
  {
    return lua_gettop(L) - (orig_top -nargs -1);
  }
  else
  {
    const char *message = lua_tostring(L, -1);
    return luaL_error(L, message);
  }
}


static int32_t do_chunk_later_callback(dt_job_t *job)
{
  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  int reference = GPOINTER_TO_INT(dt_control_job_get_params(job));
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  lua_pushinteger(L,reference);
  lua_gettable(L,-2);
  lua_State* thread = lua_tothread(L,-1);
  lua_pop(L,2);
  dt_lua_do_chunk_silent(thread,lua_gettop(thread)-1,0);
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  lua_pushinteger(L,reference);
  lua_pushnil(L);
  lua_settable(L,-3);
  lua_pop(L,1);
  dt_lua_unlock();
  return 0;
}


void dt_lua_do_chunk_later_internal(const char* function, int line,lua_State *L, int nargs)
{
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : %s called from %s %d\n",__FUNCTION__,function,line);
#endif
  
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  lua_State *new_thread = lua_newthread(L);
  const int reference = luaL_ref(L,-2);
  lua_pop(L,1);
  lua_xmove(L,new_thread,nargs+1);
  dt_job_t *job = dt_control_job_create(&do_chunk_later_callback, "lua: later_chunk");

  if(job)
  {
    dt_control_job_set_params(job, GINT_TO_POINTER(reference), NULL);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  }
}

static int dispatch_cb(lua_State *L)
{
  dt_lua_do_chunk_later(L,lua_gettop(L)-1);
  return 0;
}

static int ending_cb(lua_State *L)
{
  lua_pushboolean(L,darktable.lua_state.ending);
  return 1;
}

typedef struct gtk_wrap_communication {
  GCond end_cond;
  GMutex end_mutex;
  lua_State *L;
  int retval;
} gtk_wrap_communication;

gboolean dt_lua_gtk_wrap_callback(gpointer data)
{
  gtk_wrap_communication *communication = (gtk_wrap_communication*)data;
  g_mutex_lock(&communication->end_mutex);
  communication->retval = dt_lua_do_chunk(communication->L,lua_gettop(communication->L)-1,LUA_MULTRET);
  g_cond_signal(&communication->end_cond);
  g_mutex_unlock(&communication->end_mutex);
  return false;
} 

int dt_lua_gtk_wrap(lua_State*L)
{ 
  lua_pushvalue(L,lua_upvalueindex(1));
  lua_insert(L,1);
  if(pthread_equal(darktable.control->gui_thread, pthread_self())) {
    return dt_lua_do_chunk_raise(L,lua_gettop(L)-1,LUA_MULTRET);
  } else {
    gtk_wrap_communication communication;
    g_mutex_init(&communication.end_mutex);
    g_cond_init(&communication.end_cond);
    communication.L = L;
    g_mutex_lock(&communication.end_mutex);
    g_main_context_invoke(NULL,dt_lua_gtk_wrap_callback,&communication);
    g_cond_wait(&communication.end_cond,&communication.end_mutex);
    g_mutex_unlock(&communication.end_mutex);
    g_mutex_clear(&communication.end_mutex);
    if(communication.retval == LUA_OK) {
      return lua_gettop(L);
    } else {
      return lua_error(L);
    }
  }

}



typedef struct {
  lua_CFunction pusher;
  GList* extra;
}async_call_data;

static void async_callback_job_destructor(void *data_ptr)
{
  async_call_data* data = data_ptr;

  GList* cur_elt = data->extra;
  while(cur_elt) {
    GList * type_type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    //GList * type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    GList * data_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    switch(GPOINTER_TO_INT(type_type_elt->data)) {
      case LUA_ASYNC_TYPEID_WITH_FREE:
      case LUA_ASYNC_TYPENAME_WITH_FREE:
        {
          GList *destructor_elt = cur_elt;
          cur_elt = g_list_next(cur_elt);
          GValue to_free = G_VALUE_INIT;
          g_value_init(&to_free,G_TYPE_POINTER);
          g_value_set_pointer(&to_free,data_elt->data);
          g_closure_invoke(destructor_elt->data,NULL,1,&to_free,NULL);
          g_closure_unref (destructor_elt->data);
        }
        break;
      case LUA_ASYNC_TYPEID:
      case LUA_ASYNC_TYPENAME:
        break;
      case LUA_ASYNC_DONE:
      default:
        // should never happen
        printf("%d\n",GPOINTER_TO_INT(type_type_elt->data));
        g_assert(false);
        break;
    }
  }
  g_list_free(data->extra);
  free(data);
}
static int32_t async_callback_job(dt_job_t *job)
{
  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  async_call_data* data = (async_call_data*)dt_control_job_get_params(job);
  lua_pushcfunction(L,data->pusher);
  int nargs =0;

  GList* cur_elt = data->extra;
  while(cur_elt) {
    GList * type_type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    GList * type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    GList * data_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    switch(GPOINTER_TO_INT(type_type_elt->data)) {
      case LUA_ASYNC_TYPEID_WITH_FREE:
        // skip the destructor
        cur_elt = g_list_next(cur_elt);
        // do not break
      case LUA_ASYNC_TYPEID:
        luaA_push_type(L,GPOINTER_TO_INT(type_elt->data),data_elt->data);
        break;
      case LUA_ASYNC_TYPENAME_WITH_FREE:
        // skip the destructor
        cur_elt = g_list_next(cur_elt);
        // do not break
      case LUA_ASYNC_TYPENAME:
        luaA_push_type(L,luaA_type_find(L,type_elt->data),&data_elt->data);
        break;
      case LUA_ASYNC_DONE:
      default:
        // should never happen
        g_assert(false);
        break;
    }
    nargs++;
  }
  dt_lua_do_chunk_silent(L,nargs,0);
  dt_lua_redraw_screen();
  dt_lua_unlock();
  return 0;
}

void dt_lua_do_chunk_async_internal(const char * call_function, int line, lua_CFunction pusher,dt_lua_async_call_arg_type arg_type,...)
{
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : %s called from %s %d\n",__FUNCTION__,call_function,line);
#endif
  dt_job_t *job = dt_control_job_create(&async_callback_job, "lua: async call");
  if(job)
  {
    async_call_data*data = malloc(sizeof(async_call_data));
    data->pusher = pusher;
    data->extra=NULL;
    va_list ap;
    va_start(ap,arg_type);
    dt_lua_async_call_arg_type cur_type = arg_type;
    while(cur_type != LUA_ASYNC_DONE){
      data->extra=g_list_append(data->extra,GINT_TO_POINTER(cur_type));
      switch(cur_type) {
        case LUA_ASYNC_TYPEID:
          data->extra=g_list_append(data->extra,GINT_TO_POINTER(va_arg(ap,luaA_Type)));
          data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
          break;
        case LUA_ASYNC_TYPEID_WITH_FREE:
          {
            data->extra=g_list_append(data->extra,GINT_TO_POINTER(va_arg(ap,luaA_Type)));
            data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
            GClosure* closure = va_arg(ap,GClosure*);
            g_closure_ref (closure);
            g_closure_sink (closure);
            g_closure_set_marshal(closure, g_cclosure_marshal_generic);
            data->extra=g_list_append(data->extra,closure);
          }
          break;
        case LUA_ASYNC_TYPENAME:
          data->extra=g_list_append(data->extra,va_arg(ap,char *));
          data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
          break;
        case LUA_ASYNC_TYPENAME_WITH_FREE:
          {
            data->extra=g_list_append(data->extra,va_arg(ap,char *));
            data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
            GClosure* closure = va_arg(ap,GClosure*);
            g_closure_ref (closure);
            g_closure_sink (closure);
            g_closure_set_marshal(closure, g_cclosure_marshal_generic);
            data->extra=g_list_append(data->extra,closure);
          }
          break;
        default:
          // should never happen
          g_assert(false);
          break;
      }
      cur_type = va_arg(ap,dt_lua_async_call_arg_type);

    }
    va_end(ap);
    
    dt_control_job_set_params(job, data, async_callback_job_destructor);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  }
}
int dt_lua_init_call(lua_State *L)
{
  luaA_enum(L, yield_type);
  luaA_enum_value(L, yield_type, WAIT_MS);
  luaA_enum_value(L, yield_type, FILE_READABLE);
  luaA_enum_value(L, yield_type, RUN_COMMAND);

  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "control", NULL);
  lua_setfield(L, -2, "control");
  lua_pop(L, 1);


  
  lua_pushcfunction(L, ending_cb);
  dt_lua_type_register_const_type(L, type_id, "ending");
  lua_pushcfunction(L, dispatch_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "dispatch");
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
