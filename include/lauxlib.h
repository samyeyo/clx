// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  lauxlib.h · Lua 5.5 auxiliary API for clx  │
// └─────────────────────────────────────────────┘

#ifndef lauxlib_h
#define lauxlib_h

#include <stddef.h>
#include <stdio.h>

#include "lua.h"

#define LUA_GNAME	"_G"

typedef struct luaL_Buffer luaL_Buffer;

#define LUA_ERRFILE     (LUA_ERRERR+1)

#define LUA_LOADED_TABLE	"_LOADED"

#define LUA_PRELOAD_TABLE	"_PRELOAD"

//------------------ Function registration entry (mirrors stock lauxlib.h)
typedef struct luaL_Reg {
  const char *name;
  lua_CFunction func;
} luaL_Reg;

#define LUAL_NUMSIZES	(sizeof(lua_Integer)*16 + sizeof(lua_Number))

//------------------ Buffer alignment/size config (mirrors luaconf.h)
#ifndef LUAI_MAXALIGN
#define LUAI_MAXALIGN  lua_Number n; double u; void *s; lua_Integer i; long l
#endif

#ifndef LUAL_BUFFERSIZE
#define LUAL_BUFFERSIZE   ((int)(16 * sizeof(void*) * sizeof(lua_Number)))
#endif

//------------------ Symbol prefix: every clx auxlib symbol is clx_luaL_*
#define luaL_checkversion_ clx_luaL_checkversion_
#define luaL_getmetafield clx_luaL_getmetafield
#define luaL_callmeta clx_luaL_callmeta
#define luaL_tolstring clx_luaL_tolstring
#define luaL_argerror clx_luaL_argerror
#define luaL_typeerror clx_luaL_typeerror
#define luaL_checklstring clx_luaL_checklstring
#define luaL_optlstring clx_luaL_optlstring
#define luaL_checknumber clx_luaL_checknumber
#define luaL_optnumber clx_luaL_optnumber
#define luaL_checkinteger clx_luaL_checkinteger
#define luaL_optinteger clx_luaL_optinteger
#define luaL_checkstack clx_luaL_checkstack
#define luaL_checktype clx_luaL_checktype
#define luaL_checkany clx_luaL_checkany
#define luaL_newmetatable clx_luaL_newmetatable
#define luaL_setmetatable clx_luaL_setmetatable
#define luaL_testudata clx_luaL_testudata
#define luaL_checkudata clx_luaL_checkudata
#define luaL_where clx_luaL_where
#define luaL_error clx_luaL_error
#define luaL_checkoption clx_luaL_checkoption
#define luaL_fileresult clx_luaL_fileresult
#define luaL_execresult clx_luaL_execresult
#define luaL_alloc clx_luaL_alloc
#define luaL_ref clx_luaL_ref
#define luaL_unref clx_luaL_unref
#define luaL_loadfilex clx_luaL_loadfilex
#define luaL_loadbufferx clx_luaL_loadbufferx
#define luaL_loadstring clx_luaL_loadstring
#define luaL_newstate clx_luaL_newstate
#define luaL_makeseed clx_luaL_makeseed
#define luaL_len clx_luaL_len
#define luaL_addgsub clx_luaL_addgsub
#define luaL_gsub clx_luaL_gsub
#define luaL_setfuncs clx_luaL_setfuncs
#define luaL_getsubtable clx_luaL_getsubtable
#define luaL_traceback clx_luaL_traceback
#define luaL_requiref clx_luaL_requiref
#define luaL_buffinit clx_luaL_buffinit
#define luaL_prepbuffsize clx_luaL_prepbuffsize
#define luaL_addlstring clx_luaL_addlstring
#define luaL_addstring clx_luaL_addstring
#define luaL_addvalue clx_luaL_addvalue
#define luaL_pushresult clx_luaL_pushresult
#define luaL_pushresultsize clx_luaL_pushresultsize
#define luaL_buffinitsize clx_luaL_buffinitsize

LUALIB_API void (luaL_checkversion_) (lua_State *L, lua_Number ver, size_t sz);
#define luaL_checkversion(L)  \
	  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES)

LUALIB_API int (luaL_getmetafield) (lua_State *L, int obj, const char *e);
LUALIB_API int (luaL_callmeta) (lua_State *L, int obj, const char *e);
LUALIB_API const char *(luaL_tolstring) (lua_State *L, int idx, size_t *len);
LUALIB_API int (luaL_argerror) (lua_State *L, int arg, const char *extramsg);
LUALIB_API int (luaL_typeerror) (lua_State *L, int arg, const char *tname);
LUALIB_API const char *(luaL_checklstring) (lua_State *L, int arg,
                                                          size_t *l);
LUALIB_API const char *(luaL_optlstring) (lua_State *L, int arg,
                                          const char *def, size_t *l);
LUALIB_API lua_Number (luaL_checknumber) (lua_State *L, int arg);
LUALIB_API lua_Number (luaL_optnumber) (lua_State *L, int arg, lua_Number def);

LUALIB_API lua_Integer (luaL_checkinteger) (lua_State *L, int arg);
LUALIB_API lua_Integer (luaL_optinteger) (lua_State *L, int arg,
                                          lua_Integer def);

LUALIB_API void (luaL_checkstack) (lua_State *L, int sz, const char *msg);
LUALIB_API void (luaL_checktype) (lua_State *L, int arg, int t);
LUALIB_API void (luaL_checkany) (lua_State *L, int arg);

LUALIB_API int   (luaL_newmetatable) (lua_State *L, const char *tname);
LUALIB_API void  (luaL_setmetatable) (lua_State *L, const char *tname);
LUALIB_API void *(luaL_testudata) (lua_State *L, int ud, const char *tname);
LUALIB_API void *(luaL_checkudata) (lua_State *L, int ud, const char *tname);

LUALIB_API void (luaL_where) (lua_State *L, int lvl);
LUALIB_API int  (luaL_error) (lua_State *L, const char *fmt, ...);

LUALIB_API int (luaL_checkoption) (lua_State *L, int arg, const char *def,
                                   const char *const lst[]);

LUALIB_API int (luaL_fileresult) (lua_State *L, int stat, const char *fname);
LUALIB_API int (luaL_execresult) (lua_State *L, int stat);

LUALIB_API void *luaL_alloc (void *ud, void *ptr, size_t osize,
                                                  size_t nsize);

#define LUA_NOREF       (-2)
#define LUA_REFNIL      (-1)

LUALIB_API int (luaL_ref) (lua_State *L, int t);
LUALIB_API void (luaL_unref) (lua_State *L, int t, int ref);

LUALIB_API int (luaL_loadfilex) (lua_State *L, const char *filename,
                                               const char *mode);

#define luaL_loadfile(L,f)	luaL_loadfilex(L,f,NULL)

LUALIB_API int (luaL_loadbufferx) (lua_State *L, const char *buff, size_t sz,
                                               const char *name, const char *mode);
LUALIB_API int (luaL_loadstring) (lua_State *L, const char *s);

LUALIB_API lua_State *(luaL_newstate) (void);

LUALIB_API unsigned luaL_makeseed (lua_State *L);

LUALIB_API lua_Integer (luaL_len) (lua_State *L, int idx);

LUALIB_API void (luaL_addgsub) (luaL_Buffer *b, const char *s,
                                     const char *p, const char *r);
LUALIB_API const char *(luaL_gsub) (lua_State *L, const char *s,
                                    const char *p, const char *r);

LUALIB_API void (luaL_setfuncs) (lua_State *L, const luaL_Reg *l, int nup);

LUALIB_API int (luaL_getsubtable) (lua_State *L, int idx, const char *fname);

LUALIB_API void (luaL_traceback) (lua_State *L, lua_State *L1,
                                  const char *msg, int level);

LUALIB_API void (luaL_requiref) (lua_State *L, const char *modname,
                                 lua_CFunction openf, int glb);

//------------------ Useful macros
#define luaL_newlibtable(L,l)	\
  lua_createtable(L, 0, sizeof(l)/sizeof((l)[0]) - 1)

#define luaL_newlib(L,l)  \
  (luaL_checkversion(L), luaL_newlibtable(L,l), luaL_setfuncs(L,l,0))

#define luaL_argcheck(L, cond,arg,extramsg)	\
	((void)(luai_likely(cond) || luaL_argerror(L, (arg), (extramsg))))

#define luaL_argexpected(L,cond,arg,tname)	\
	((void)(luai_likely(cond) || luaL_typeerror(L, (arg), (tname))))

#define luaL_checkstring(L,n)	(luaL_checklstring(L, (n), NULL))
#define luaL_optstring(L,n,d)	(luaL_optlstring(L, (n), (d), NULL))

#define luaL_typename(L,i)	lua_typename(L, lua_type(L,(i)))

#define luaL_dofile(L, fn) \
	(luaL_loadfile(L, fn) || lua_pcall(L, 0, LUA_MULTRET, 0))

#define luaL_dostring(L, s) \
	(luaL_loadstring(L, s) || lua_pcall(L, 0, LUA_MULTRET, 0))

#define luaL_getmetatable(L,n)	(lua_getfield(L, LUA_REGISTRYINDEX, (n)))

#define luaL_opt(L,f,n,d)	(lua_isnoneornil(L,(n)) ? (d) : f(L,(n)))

#define luaL_loadbuffer(L,s,sz,n)	luaL_loadbufferx(L,s,sz,n,NULL)

#define luaL_intop(op,v1,v2)  \
	((lua_Integer)((lua_Unsigned)(v1) op (lua_Unsigned)(v2)))

#if defined(LUA_FAILISFALSE)
#define luaL_pushfail(L)	lua_pushboolean(L, 0)
#else
#define luaL_pushfail(L)	lua_pushnil(L)
#endif

//------------------ Generic buffer manipulation
struct luaL_Buffer {
  char *b;
  size_t size;
  size_t n;
  lua_State *L;
  void *ext;
  union {
    LUAI_MAXALIGN;
    char b[LUAL_BUFFERSIZE];
  } init;
};

#define luaL_bufflen(bf)	((bf)->n)
#define luaL_buffaddr(bf)	((bf)->b)

#define luaL_addchar(B,c) \
  ((void)((B)->n < (B)->size || luaL_prepbuffsize((B), 1)), \
   ((B)->b[(B)->n++] = (c)))

#define luaL_addsize(B,s)	((B)->n += (s))

#define luaL_buffsub(B,s)	((B)->n -= (s))

LUALIB_API void (luaL_buffinit) (lua_State *L, luaL_Buffer *B);
LUALIB_API char *(luaL_prepbuffsize) (luaL_Buffer *B, size_t sz);
LUALIB_API void (luaL_addlstring) (luaL_Buffer *B, const char *s, size_t l);
LUALIB_API void (luaL_addstring) (luaL_Buffer *B, const char *s);
LUALIB_API void (luaL_addvalue) (luaL_Buffer *B);
LUALIB_API void (luaL_pushresult) (luaL_Buffer *B);
LUALIB_API void (luaL_pushresultsize) (luaL_Buffer *B, size_t sz);
LUALIB_API char *(luaL_buffinitsize) (lua_State *L, luaL_Buffer *B, size_t sz);

#define luaL_prepbuffer(B)	luaL_prepbuffsize(B, LUAL_BUFFERSIZE)

//------------------ File handles for the IO library
#define LUA_FILEHANDLE          "FILE*"

typedef struct luaL_Stream {
  FILE *f;
  lua_CFunction closef;
} luaL_Stream;

#if defined(LUA_COMPAT_APIINTCASTS)

#define luaL_checkunsigned(L,a)	((lua_Unsigned)luaL_checkinteger(L,(a)))
#define luaL_optunsigned(L,a,d)	\
	((lua_Unsigned)luaL_optinteger(L,(a),(lua_Integer)(d)))

#define luaL_checkint(L,n)	((int)luaL_checkinteger(L, (n)))
#define luaL_optint(L,n,d)	((int)luaL_optinteger(L, (n), (d)))

#define luaL_checklong(L,n)	((long)luaL_checkinteger(L, (n)))
#define luaL_optlong(L,n,d)	((long)luaL_optinteger(L, (n), (d)))

#endif

#endif
