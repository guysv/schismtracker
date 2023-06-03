/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
 * copyright (c) 2003-2005 Storlek <storlek@rigelseven.com>
 * copyright (c) 2005-2008 Mrs. Brisby <mrs.brisby@nimh.org>
 * copyright (c) 2009 Storlek & Mrs. Brisby
 * copyright (c) 2010-2012 Storlek
 * URL: http://schismtracker.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA    02111-1307    USA
 */

#include "headers.h"

#include "it.h"
#include "util.h"
#include "song.h"
#include "lua-patternlib.h"
#include "lua-engine.h"

#include <assert.h>
#include <stdarg.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static lua_State *L;
static lua_console_write console_write = NULL;

#define running (lua_status(L) == LUA_YIELD)
static int interrupt = 0;
static int yield_ret = 0;

extern const unsigned char taskqueue_script[];
extern size_t taskqueue_script_size;

static char *rc_filename = NULL;

void set_lua_print(lua_console_write write_func)
{
	console_write = write_func;
}

static int lua_print_console(lua_State *L) {
    int n = lua_gettop(L);
    int i;

    if (!console_write)
		return 0;

    for (i = 1; i <= n; i++) {
	size_t l;
	const char *s = luaL_tolstring(L, i, &l);
	if (i > 1)
	    console_write("\t", 1);
	console_write(s, l);
	lua_pop(L, 1);
    }
    console_write("\n", 1);
    return 0;
}

static void multitask_hook(lua_State *L, lua_Debug *ar) {
	lua_yield(L, lua_gettop(L));
}

void do_lua_resume(int args) {
	int n;
	const char *err;

	status.flags |= NEED_UPDATE;

	lua_sethook(L, multitask_hook, LUA_MASKCOUNT, 1000);

	switch (lua_resume(L, NULL, args, &yield_ret)) {
	case LUA_YIELD:
		return;
	default:
	case LUA_OK:
		lua_resetthread(L);

		n = lua_gettop(L);
		if (n > 0) {
			lua_getglobal(L, "print");
			lua_insert(L, 1);
			lua_call(L, n, 0);
		}

		break;
	}
}

void eval_lua_input(char *input) {
	int n;

	if (running) {
		return;
	}

	luaL_loadstring(L, input);
	do_lua_resume(0);
}

void continue_lua_eval() {
	int nres;

	if (running) {
		do_lua_resume(yield_ret);
		return;
	}

	lua_sethook(L, NULL, 0, 0);
	lua_getglobal(L, "_pop_task");
	lua_call(L, 0, LUA_MULTRET);
	if (!lua_isnil(L, 1)) {
		do_lua_resume(lua_gettop(L)-1);
	} else {
		lua_pop(L, 1);
	}
}

void push_lua_task_ints(char *cb, int nargs, ...)
{
	va_list args;
	lua_getglobal(L, "_push_task");

	lua_getglobal(L, cb);
	if (lua_isnil(L, -1)) {
		lua_pop(L, -2);
		return;
	}

	va_start(args, nargs);

	for (int i = 0; i < nargs; i++)
		lua_pushinteger(L, va_arg(args, int));

	va_end(args);

	lua_sethook(L, NULL, 0, 0);
	lua_call(L, nargs+1 /* 1 = cb */, 0);
}

void push_lua_midi_cc_task(int value, int param)
{
	push_lua_task_ints("_on_midi_cc", 2, value, param);
}

void push_lua_midi_note_task(int note, int velocity)
{
	push_lua_task_ints("_on_midi_note", 2, note, velocity);
}

void push_lua_playback_update_task(int pattern, int row)
{
	push_lua_task_ints("_on_playback_update", 2, pattern, row);
}

static int lua_song_start(lua_State *L)
{
	song_start();
	return 0;
}

static int lua_song_stop(lua_State *L)
{
	song_stop();
	return 0;
}

static int lua_current_pattern(lua_State *L)
{
	lua_pushinteger(L, get_current_pattern());
	return 1;
}

static int lua_current_row(lua_State *L)
{
	lua_pushinteger(L, get_current_row());
	return 1;
}

static int lua_current_channel(lua_State *L)
{
	lua_pushinteger(L, get_current_channel());
	return 1;
}

static int lua_set_current_channel(lua_State *L)
{
	set_current_channel(lua_tointeger(L, 1));
	return 0;
}

void lua_rc_load(void)
{
	int err;
	rc_filename = getenv("SCHISM_LUA_RC");
	if (!rc_filename)
		rc_filename = dmoz_path_concat(cfg_dir_dotschism, "rc.lua");
	
	err = luaL_loadfile(L, rc_filename);
	if (err != LUA_OK) {
		if (err != LUA_ERRFILE)
			log_appendf(5, " %s", lua_tostring(L, -1));
		lua_settop(L, 0);
		return;
	}

	log_appendf(5, " loaded lua user script at %s", rc_filename);
	do_lua_resume(0);
}

void lua_init(void)
{
	L = luaL_newstate();
	if (!L) {
		fprintf(stderr, "Couldn't initialise lua!\n");
		exit(1);
	}

	luaL_openlibs(L);
	luaL_requiref(L, "pattern", luaopen_pattern, 1);
	lua_pop(L, 1);

	lua_getglobal(L, "print");
	lua_setglobal(L, "_print");

	lua_pushcfunction(L, lua_print_console);
	lua_setglobal(L, "print");

	lua_pushcfunction(L, lua_song_start);
	lua_setglobal(L, "song_start");

	lua_pushcfunction(L, lua_song_stop);
	lua_setglobal(L, "song_stop");

	lua_pushcfunction(L, lua_current_pattern);
	lua_setglobal(L, "current_pattern");

	lua_pushcfunction(L, lua_current_channel);
	lua_setglobal(L, "current_channel");

	lua_pushcfunction(L, lua_set_current_channel);
	lua_setglobal(L, "set_current_channel");

	lua_pushcfunction(L, lua_current_row);
	lua_setglobal(L, "current_row");

	// luaL_loadbuffer
	luaL_loadbuffer(L, taskqueue_script, taskqueue_script_size, "@taskqueue.lua");
	lua_pcall(L, 0, LUA_MULTRET, 0);

	log_append(2, 0, "Lua initialised");
	log_underline(15);
	lua_rc_load();
	// log_appendf(5, " loaded user script %s");
}
