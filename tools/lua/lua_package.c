/* ============================================================================
 * lua_package.c -- 裸机版的 package / require（替代 loadlib.c）
 *
 * TinyOS 没有动态加载（dlopen）机制，所以这里实现一个最小的
 * luaopen_package：
 *   - package.searchers[1] = VFS searcher：把模块名 `a.b.c` 映射到
 *     /lib/lua/a/b/c.lua（及 init.lua），经 api->file_read 读进内存，
 *     用 luaL_loadbuffer 编译成 chunk，require 调用后即得模块值。
 *   - 其余标准字段（config/loaded/path/cpath/loadlib/preload）给个占位。
 * ============================================================================ */
#include "api_user.h"
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

extern tinyos_api_t *g_lua_api;

#define LUA_BUF 8192

/* 模块名 "a.b.c" -> 路径候选；返回候选数，path 数组里填好 */
static int mod_paths(const char *mod, char (*paths)[128], int max) {
    char dot[160];
    int n = 0;
    /* 候选 1: /lib/lua/a/b/c.lua */
    {
        char p[128];
        char *o = p;
        const char *q = "/lib/lua/";
        while (*q) *o++ = *q++;
        for (const char *s = mod; *s; s++) {
            *o++ = (*s == '.') ? '/' : *s;
        }
        *o++ = '.'; *o++ = 'l'; *o++ = 'u'; *o++ = 'a'; *o = 0;
        strncpy(paths[n++], p, 127);
    }
    /* 候选 2: /lib/lua/a/b/c/init.lua */
    {
        char p[128];
        char *o = p;
        const char *q = "/lib/lua/";
        while (*q) *o++ = *q++;
        for (const char *s = mod; *s; s++) {
            *o++ = (*s == '.') ? '/' : *s;
        }
        strcpy(o, "/init.lua");
        strncpy(paths[n++], p, 127);
    }
    (void)dot; (void)max;
    return n;
}

/* VFS searcher：返回已编译的 chunk 函数 + chunkname；失败返回错误串 */
static int vfs_searcher(lua_State *L) {
    const char *mod = luaL_checkstring(L, 1);
    char paths[4][128];
    int n = mod_paths(mod, paths, 4);
    char buf[LUA_BUF];
    for (int i = 0; i < n; i++) {
        int len = g_lua_api->file_read(paths[i], buf, (int)sizeof buf - 1);
        if (len > 0) {
            buf[len] = 0;
            if (luaL_loadbuffer(L, buf, (size_t)len, paths[i]) != LUA_OK) {
                return luaL_error(L, "error loading module '%s':\n  %s",
                                  mod, lua_tostring(L, -1));
            }
            lua_pushstring(L, paths[i]);   /* extra: chunkname */
            return 2;                      /* loader function + chunkname */
        }
    }
    lua_pushfstring(L, "\tno file '%s'", mod);
    return 1;
}

LUAMOD_API int luaopen_package(lua_State *L) {
    /* package 表 */
    lua_createtable(L, 0, 6);
    lua_pushliteral(L, "/.;\n");
    lua_setfield(L, -2, "config");
    lua_createtable(L, 0, 1);
    lua_setfield(L, -2, "loaded");
    lua_pushliteral(L, "/lib/lua/?.lua;/lib/lua/?/init.lua");
    lua_setfield(L, -2, "path");
    lua_pushliteral(L, "/lib/lua/?.so");
    lua_setfield(L, -2, "cpath");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "loadlib");
    lua_createtable(L, 0, 1);
    lua_setfield(L, -2, "preload");
    /* searchers */
    lua_createtable(L, 2, 0);
    lua_pushvalue(L, -2);                 /* upvalue = package */
    lua_pushcclosure(L, vfs_searcher, 1);
    lua_rawseti(L, -3, 1);
    lua_setfield(L, -2, "searchers");
    return 1;
}
