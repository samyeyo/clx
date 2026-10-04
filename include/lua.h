// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  lua.h · Lua 5.5 C API for clx modules      │
// └─────────────────────────────────────────────┘

#ifndef lua_h
#define lua_h

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>

//------------------ Lua 5.5 version identifiers (mirrors stock lua.h)
#define LUA_COPYRIGHT	LUA_RELEASE "  Copyright (C) 1994-2025 Lua.org, PUC-Rio"
#define LUA_AUTHORS	"R. Ierusalimschy, L. H. de Figueiredo, W. Celes"

#define LUA_VERSION_MAJOR_N	5
#define LUA_VERSION_MINOR_N	5
#define LUA_VERSION_RELEASE_N	0

#define LUA_VERSION_NUM  (LUA_VERSION_MAJOR_N * 100 + LUA_VERSION_MINOR_N)
#define LUA_VERSION_RELEASE_NUM  (LUA_VERSION_NUM * 100 + LUA_VERSION_RELEASE_N)

#if defined(LUA_USER_H)
#include LUA_USER_H
#endif

#define LUA_SIGNATURE	"\x1bLua"

#define LUA_MULTRET	(-1)

//------------------ Pseudo-indices (registry + per-frame upvalues)
#define LUA_REGISTRYINDEX	(-(INT_MAX/2 + 1000))
#define lua_upvalueindex(i)	(LUA_REGISTRYINDEX - (i))

//------------------ Thread status codes
#define LUA_OK		0
#define LUA_YIELD	1
#define LUA_ERRRUN	2
#define LUA_ERRSYNTAX	3
#define LUA_ERRMEM	4
#define LUA_ERRERR	5

typedef struct lua_State lua_State;

//------------------ Basic type tags
#define LUA_TNONE		(-1)

#define LUA_TNIL		0
#define LUA_TBOOLEAN		1
#define LUA_TLIGHTUSERDATA	2
#define LUA_TNUMBER		3
#define LUA_TSTRING		4
#define LUA_TTABLE		5
#define LUA_TFUNCTION		6
#define LUA_TUSERDATA		7
#define LUA_TTHREAD		8

#define LUA_NUMTYPES		9

#define LUA_MINSTACK	20

//------------------ Predefined registry values
#define LUA_RIDX_GLOBALS	2
#define LUA_RIDX_MAINTHREAD	3
#define LUA_RIDX_LAST		3

//------------------ Number/integer types (clx: double + int64, matching LValue)
#ifndef LUA_NUMBER
#define LUA_NUMBER	double
#endif

#ifndef LUA_INTEGER
#define LUA_INTEGER	long long
#endif

#ifndef LUA_UNSIGNED
#define LUA_UNSIGNED	unsigned long long
#endif

#ifndef LUA_KCONTEXT
#define LUA_KCONTEXT	ptrdiff_t
#endif

//------------------ Raw memory area attached to the state (mirrors luaconf.h)
#ifndef LUA_EXTRASPACE
#define LUA_EXTRASPACE	(sizeof(void *) * 4)
#endif

#ifndef LUA_IDSIZE
#define LUA_IDSIZE	60
#endif

typedef LUA_NUMBER lua_Number;
typedef LUA_INTEGER lua_Integer;
typedef LUA_UNSIGNED lua_Unsigned;
typedef LUA_KCONTEXT lua_KContext;

//------------------ luai_likely: branch hint used by luaL_argcheck (mirrors luaconf.h)
#if !defined(luai_likely)
#if defined(__GNUC__)
#define luai_likely(x)		(__builtin_expect(((x) != 0), 1))
#else
#define luai_likely(x)		(x)
#endif
#endif

//------------------ C function / continuation / IO callback types
typedef int (*lua_CFunction) (lua_State *L);

typedef int (*lua_KFunction) (lua_State *L, int status, lua_KContext ctx);

typedef const char * (*lua_Reader) (lua_State *L, void *ud, size_t *sz);

typedef int (*lua_Writer) (lua_State *L, const void *p, size_t sz, void *ud);

typedef void * (*lua_Alloc) (void *ud, void *ptr, size_t osize, size_t nsize);

typedef void (*lua_WarnFunction) (void *ud, const char *msg, int tocont);

typedef struct lua_Debug lua_Debug;

typedef void (*lua_Hook) (lua_State *L, lua_Debug *ar);

//------------------ Extern linkage: C modules compile as C, the bridge is C++
#ifndef LUA_API
#ifdef __cplusplus
#define LUA_API	extern "C"
#else
#define LUA_API	extern
#endif
#endif

#ifndef LUALIB_API
#define LUALIB_API	LUA_API
#endif

#ifndef LUAMOD_API
#ifdef __cplusplus
#define LUAMOD_API	extern "C"
#else
#define LUAMOD_API	extern
#endif
#endif

//------------------ Symbol prefix: every clx C API symbol is clx_lua_* so a C module
//------------------ never collides with a real Lua VM linked by clx --dynamic.
#define lua_newstate clx_lua_newstate
#define lua_close clx_lua_close
#define lua_newthread clx_lua_newthread
#define lua_closethread clx_lua_closethread
#define lua_atpanic clx_lua_atpanic
#define lua_version clx_lua_version
#define lua_absindex clx_lua_absindex
#define lua_gettop clx_lua_gettop
#define lua_settop clx_lua_settop
#define lua_pushvalue clx_lua_pushvalue
#define lua_rotate clx_lua_rotate
#define lua_copy clx_lua_copy
#define lua_checkstack clx_lua_checkstack
#define lua_xmove clx_lua_xmove
#define lua_isnumber clx_lua_isnumber
#define lua_isstring clx_lua_isstring
#define lua_iscfunction clx_lua_iscfunction
#define lua_isinteger clx_lua_isinteger
#define lua_isuserdata clx_lua_isuserdata
#define lua_type clx_lua_type
#define lua_typename clx_lua_typename
#define lua_tonumberx clx_lua_tonumberx
#define lua_tointegerx clx_lua_tointegerx
#define lua_toboolean clx_lua_toboolean
#define lua_tolstring clx_lua_tolstring
#define lua_rawlen clx_lua_rawlen
#define lua_tocfunction clx_lua_tocfunction
#define lua_touserdata clx_lua_touserdata
#define lua_tothread clx_lua_tothread
#define lua_topointer clx_lua_topointer
#define lua_arith clx_lua_arith
#define lua_rawequal clx_lua_rawequal
#define lua_compare clx_lua_compare
#define lua_pushnil clx_lua_pushnil
#define lua_pushnumber clx_lua_pushnumber
#define lua_pushinteger clx_lua_pushinteger
#define lua_pushlstring clx_lua_pushlstring
#define lua_pushexternalstring clx_lua_pushexternalstring
#define lua_pushstring clx_lua_pushstring
#define lua_pushvfstring clx_lua_pushvfstring
#define lua_pushfstring clx_lua_pushfstring
#define lua_pushcclosure clx_lua_pushcclosure
#define lua_pushboolean clx_lua_pushboolean
#define lua_pushlightuserdata clx_lua_pushlightuserdata
#define lua_pushthread clx_lua_pushthread
#define lua_getglobal clx_lua_getglobal
#define lua_gettable clx_lua_gettable
#define lua_getfield clx_lua_getfield
#define lua_geti clx_lua_geti
#define lua_rawget clx_lua_rawget
#define lua_rawgeti clx_lua_rawgeti
#define lua_rawgetp clx_lua_rawgetp
#define lua_createtable clx_lua_createtable
#define lua_newuserdatauv clx_lua_newuserdatauv
#define lua_getmetatable clx_lua_getmetatable
#define lua_getiuservalue clx_lua_getiuservalue
#define lua_setglobal clx_lua_setglobal
#define lua_settable clx_lua_settable
#define lua_setfield clx_lua_setfield
#define lua_seti clx_lua_seti
#define lua_rawset clx_lua_rawset
#define lua_rawseti clx_lua_rawseti
#define lua_rawsetp clx_lua_rawsetp
#define lua_setmetatable clx_lua_setmetatable
#define lua_setiuservalue clx_lua_setiuservalue
#define lua_callk clx_lua_callk
#define lua_pcallk clx_lua_pcallk
#define lua_load clx_lua_load
#define lua_dump clx_lua_dump
#define lua_yieldk clx_lua_yieldk
#define lua_resume clx_lua_resume
#define lua_status clx_lua_status
#define lua_isyieldable clx_lua_isyieldable
#define lua_setwarnf clx_lua_setwarnf
#define lua_warning clx_lua_warning
#define lua_gc clx_lua_gc
#define lua_error clx_lua_error
#define lua_next clx_lua_next
#define lua_concat clx_lua_concat
#define lua_len clx_lua_len
#define lua_numbertocstring clx_lua_numbertocstring
#define lua_stringtonumber clx_lua_stringtonumber
#define lua_getallocf clx_lua_getallocf
#define lua_setallocf clx_lua_setallocf
#define lua_toclose clx_lua_toclose
#define lua_closeslot clx_lua_closeslot
#define lua_getstack clx_lua_getstack
#define lua_getinfo clx_lua_getinfo
#define lua_getlocal clx_lua_getlocal
#define lua_setlocal clx_lua_setlocal
#define lua_getupvalue clx_lua_getupvalue
#define lua_setupvalue clx_lua_setupvalue
#define lua_upvalueid clx_lua_upvalueid
#define lua_upvaluejoin clx_lua_upvaluejoin
#define lua_sethook clx_lua_sethook
#define lua_gethook clx_lua_gethook
#define lua_gethookmask clx_lua_gethookmask
#define lua_gethookcount clx_lua_gethookcount
#define lua_ident clx_lua_ident

//------------------ RCS ident string (LUA_API: C++ side must stay extern "C" so the
//------------------ symbol links against the bridge's definition, not a mangled name)
LUA_API const char lua_ident[];

//------------------ State manipulation
LUA_API lua_State *(lua_newstate) (lua_Alloc f, void *ud, unsigned seed);
LUA_API void       (lua_close) (lua_State *L);
LUA_API lua_State *(lua_newthread) (lua_State *L);
LUA_API int        (lua_closethread) (lua_State *L, lua_State *from);

LUA_API lua_CFunction (lua_atpanic) (lua_State *L, lua_CFunction panicf);

LUA_API lua_Number (lua_version) (lua_State *L);

//------------------ Basic stack manipulation
LUA_API int   (lua_absindex) (lua_State *L, int idx);
LUA_API int   (lua_gettop) (lua_State *L);
LUA_API void  (lua_settop) (lua_State *L, int idx);
LUA_API void  (lua_pushvalue) (lua_State *L, int idx);
LUA_API void  (lua_rotate) (lua_State *L, int idx, int n);
LUA_API void  (lua_copy) (lua_State *L, int fromidx, int toidx);
LUA_API int   (lua_checkstack) (lua_State *L, int n);

LUA_API void  (lua_xmove) (lua_State *from, lua_State *to, int n);

//------------------ Access functions (stack -> C)
LUA_API int             (lua_isnumber) (lua_State *L, int idx);
LUA_API int             (lua_isstring) (lua_State *L, int idx);
LUA_API int             (lua_iscfunction) (lua_State *L, int idx);
LUA_API int             (lua_isinteger) (lua_State *L, int idx);
LUA_API int             (lua_isuserdata) (lua_State *L, int idx);
LUA_API int             (lua_type) (lua_State *L, int idx);
LUA_API const char     *(lua_typename) (lua_State *L, int tp);

LUA_API lua_Number      (lua_tonumberx) (lua_State *L, int idx, int *isnum);
LUA_API lua_Integer     (lua_tointegerx) (lua_State *L, int idx, int *isnum);
LUA_API int             (lua_toboolean) (lua_State *L, int idx);
LUA_API const char     *(lua_tolstring) (lua_State *L, int idx, size_t *len);
LUA_API lua_Unsigned    (lua_rawlen) (lua_State *L, int idx);
LUA_API lua_CFunction   (lua_tocfunction) (lua_State *L, int idx);
LUA_API void	       *(lua_touserdata) (lua_State *L, int idx);
LUA_API lua_State      *(lua_tothread) (lua_State *L, int idx);
LUA_API const void     *(lua_topointer) (lua_State *L, int idx);

//------------------ Comparison and arithmetic
#define LUA_OPADD	0
#define LUA_OPSUB	1
#define LUA_OPMUL	2
#define LUA_OPMOD	3
#define LUA_OPPOW	4
#define LUA_OPDIV	5
#define LUA_OPIDIV	6
#define LUA_OPBAND	7
#define LUA_OPBOR	8
#define LUA_OPBXOR	9
#define LUA_OPSHL	10
#define LUA_OPSHR	11
#define LUA_OPUNM	12
#define LUA_OPBNOT	13

LUA_API void  (lua_arith) (lua_State *L, int op);

#define LUA_OPEQ	0
#define LUA_OPLT	1
#define LUA_OPLE	2

LUA_API int   (lua_rawequal) (lua_State *L, int idx1, int idx2);
LUA_API int   (lua_compare) (lua_State *L, int idx1, int idx2, int op);

//------------------ Push functions (C -> stack)
LUA_API void        (lua_pushnil) (lua_State *L);
LUA_API void        (lua_pushnumber) (lua_State *L, lua_Number n);
LUA_API void        (lua_pushinteger) (lua_State *L, lua_Integer n);
LUA_API const char *(lua_pushlstring) (lua_State *L, const char *s, size_t len);
LUA_API const char *(lua_pushexternalstring) (lua_State *L,
		const char *s, size_t len, lua_Alloc falloc, void *ud);
LUA_API const char *(lua_pushstring) (lua_State *L, const char *s);
LUA_API const char *(lua_pushvfstring) (lua_State *L, const char *fmt,
                                                      va_list argp);
LUA_API const char *(lua_pushfstring) (lua_State *L, const char *fmt, ...);
LUA_API void  (lua_pushcclosure) (lua_State *L, lua_CFunction fn, int n);
LUA_API void  (lua_pushboolean) (lua_State *L, int b);
LUA_API void  (lua_pushlightuserdata) (lua_State *L, void *p);
LUA_API int   (lua_pushthread) (lua_State *L);

//------------------ Get functions (Lua -> stack)
LUA_API int (lua_getglobal) (lua_State *L, const char *name);
LUA_API int (lua_gettable) (lua_State *L, int idx);
LUA_API int (lua_getfield) (lua_State *L, int idx, const char *k);
LUA_API int (lua_geti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawget) (lua_State *L, int idx);
LUA_API int (lua_rawgeti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawgetp) (lua_State *L, int idx, const void *p);

LUA_API void  (lua_createtable) (lua_State *L, int narr, int nrec);
LUA_API void *(lua_newuserdatauv) (lua_State *L, size_t sz, int nuvalue);
LUA_API int   (lua_getmetatable) (lua_State *L, int objindex);
LUA_API int   (lua_getiuservalue) (lua_State *L, int idx, int n);

//------------------ Set functions (stack -> Lua)
LUA_API void  (lua_setglobal) (lua_State *L, const char *name);
LUA_API void  (lua_settable) (lua_State *L, int idx);
LUA_API void  (lua_setfield) (lua_State *L, int idx, const char *k);
LUA_API void  (lua_seti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawset) (lua_State *L, int idx);
LUA_API void  (lua_rawseti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawsetp) (lua_State *L, int idx, const void *p);
LUA_API int   (lua_setmetatable) (lua_State *L, int objindex);
LUA_API int   (lua_setiuservalue) (lua_State *L, int idx, int n);

//------------------ Load and call functions
LUA_API void  (lua_callk) (lua_State *L, int nargs, int nresults,
                           lua_KContext ctx, lua_KFunction k);
#define lua_call(L,n,r)		lua_callk(L, (n), (r), 0, NULL)

LUA_API int   (lua_pcallk) (lua_State *L, int nargs, int nresults, int errfunc,
                            lua_KContext ctx, lua_KFunction k);
#define lua_pcall(L,n,r,f)	lua_pcallk(L, (n), (r), (f), 0, NULL)

LUA_API int   (lua_load) (lua_State *L, lua_Reader reader, void *dt,
                          const char *chunkname, const char *mode);

LUA_API int (lua_dump) (lua_State *L, lua_Writer writer, void *data, int strip);

//------------------ Coroutine functions
LUA_API int  (lua_yieldk)     (lua_State *L, int nresults, lua_KContext ctx,
                               lua_KFunction k);
LUA_API int  (lua_resume)     (lua_State *L, lua_State *from, int narg,
                               int *nres);
LUA_API int  (lua_status)     (lua_State *L);
LUA_API int (lua_isyieldable) (lua_State *L);

#define lua_yield(L,n)		lua_yieldk(L, (n), 0, NULL)

//------------------ Warning-related functions
LUA_API void (lua_setwarnf) (lua_State *L, lua_WarnFunction f, void *ud);
LUA_API void (lua_warning) (lua_State *L, const char *msg, int tocont);

//------------------ Garbage-collection options
#define LUA_GCSTOP		0
#define LUA_GCRESTART		1
#define LUA_GCCOLLECT		2
#define LUA_GCCOUNT		3
#define LUA_GCCOUNTB		4
#define LUA_GCSTEP		5
#define LUA_GCISRUNNING		6
#define LUA_GCGEN		7
#define LUA_GCINC		8
#define LUA_GCPARAM		9

#define LUA_GCPMINORMUL		0
#define LUA_GCPMAJORMINOR	1
#define LUA_GCPMINORMAJOR	2
#define LUA_GCPPAUSE		3
#define LUA_GCPSTEPMUL		4
#define LUA_GCPSTEPSIZE		5
#define LUA_GCPN		6

LUA_API int (lua_gc) (lua_State *L, int what, ...);

//------------------ Miscellaneous functions
LUA_API int   (lua_error) (lua_State *L);

LUA_API int   (lua_next) (lua_State *L, int idx);

LUA_API void  (lua_concat) (lua_State *L, int n);
LUA_API void  (lua_len)    (lua_State *L, int idx);

#define LUA_N2SBUFFSZ	64
LUA_API unsigned  (lua_numbertocstring) (lua_State *L, int idx, char *buff);
LUA_API size_t  (lua_stringtonumber) (lua_State *L, const char *s);

LUA_API lua_Alloc (lua_getallocf) (lua_State *L, void **ud);
LUA_API void      (lua_setallocf) (lua_State *L, lua_Alloc f, void *ud);

LUA_API void (lua_toclose) (lua_State *L, int idx);
LUA_API void (lua_closeslot) (lua_State *L, int idx);

//------------------ Useful macros
#define lua_getextraspace(L)	((void *)((char *)(L) - LUA_EXTRASPACE))

#define lua_tonumber(L,i)	lua_tonumberx(L,(i),NULL)
#define lua_tointeger(L,i)	lua_tointegerx(L,(i),NULL)

#define lua_pop(L,n)		lua_settop(L, -(n)-1)

#define lua_newtable(L)		lua_createtable(L, 0, 0)

#define lua_register(L,n,f) (lua_pushcfunction(L, (f)), lua_setglobal(L, (n)))

#define lua_pushcfunction(L,f)	lua_pushcclosure(L, (f), 0)

#define lua_isfunction(L,n)	(lua_type(L, (n)) == LUA_TFUNCTION)
#define lua_istable(L,n)	(lua_type(L, (n)) == LUA_TTABLE)
#define lua_islightuserdata(L,n)	(lua_type(L, (n)) == LUA_TLIGHTUSERDATA)
#define lua_isnil(L,n)		(lua_type(L, (n)) == LUA_TNIL)
#define lua_isboolean(L,n)	(lua_type(L, (n)) == LUA_TBOOLEAN)
#define lua_isthread(L,n)	(lua_type(L, (n)) == LUA_TTHREAD)
#define lua_isnone(L,n)		(lua_type(L, (n)) == LUA_TNONE)
#define lua_isnoneornil(L, n)	(lua_type(L, (n)) <= 0)

#define lua_pushliteral(L, s)	lua_pushstring(L, "" s)

#define lua_pushglobaltable(L)  \
	((void)lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS))

#define lua_tostring(L,i)	lua_tolstring(L, (i), NULL)

#define lua_insert(L,idx)	lua_rotate(L, (idx), 1)

#define lua_remove(L,idx)	(lua_rotate(L, (idx), -1), lua_pop(L, 1))

#define lua_replace(L,idx)	(lua_copy(L, -1, (idx)), lua_pop(L, 1))

//------------------ Compatibility macros
#define lua_newuserdata(L,s)	lua_newuserdatauv(L,s,1)
#define lua_getuservalue(L,idx)	lua_getiuservalue(L,idx,1)
#define lua_setuservalue(L,idx)	lua_setiuservalue(L,idx,1)

#define lua_resetthread(L)	lua_closethread(L,NULL)

//------------------ Debug API event codes
#define LUA_HOOKCALL	0
#define LUA_HOOKRET	1
#define LUA_HOOKLINE	2
#define LUA_HOOKCOUNT	3
#define LUA_HOOKTAILCALL 4

#define LUA_MASKCALL	(1 << LUA_HOOKCALL)
#define LUA_MASKRET	(1 << LUA_HOOKRET)
#define LUA_MASKLINE	(1 << LUA_HOOKLINE)
#define LUA_MASKCOUNT	(1 << LUA_HOOKCOUNT)

LUA_API int (lua_getstack) (lua_State *L, int level, lua_Debug *ar);
LUA_API int (lua_getinfo) (lua_State *L, const char *what, lua_Debug *ar);
LUA_API const char *(lua_getlocal) (lua_State *L, const lua_Debug *ar, int n);
LUA_API const char *(lua_setlocal) (lua_State *L, const lua_Debug *ar, int n);
LUA_API const char *(lua_getupvalue) (lua_State *L, int funcindex, int n);
LUA_API const char *(lua_setupvalue) (lua_State *L, int funcindex, int n);

LUA_API void *(lua_upvalueid) (lua_State *L, int fidx, int n);
LUA_API void  (lua_upvaluejoin) (lua_State *L, int fidx1, int n1,
                                               int fidx2, int n2);

LUA_API void (lua_sethook) (lua_State *L, lua_Hook func, int mask, int count);
LUA_API lua_Hook (lua_gethook) (lua_State *L);
LUA_API int (lua_gethookmask) (lua_State *L);
LUA_API int (lua_gethookcount) (lua_State *L);

struct lua_Debug {
  int event;
  const char *name;
  const char *namewhat;
  const char *what;
  const char *source;
  size_t srclen;
  int currentline;
  int linedefined;
  int lastlinedefined;
  unsigned char nups;
  unsigned char nparams;
  char isvararg;
  unsigned char extraargs;
  char istailcall;
  int ftransfer;
  int ntransfer;
  char short_src[LUA_IDSIZE];
  struct CallInfo *i_ci;
};

#define LUAI_TOSTRAUX(x)	#x
#define LUAI_TOSTR(x)		LUAI_TOSTRAUX(x)

#define LUA_VERSION_MAJOR	LUAI_TOSTR(LUA_VERSION_MAJOR_N)
#define LUA_VERSION_MINOR	LUAI_TOSTR(LUA_VERSION_MINOR_N)
#define LUA_VERSION_RELEASE	LUAI_TOSTR(LUA_VERSION_RELEASE_N)

#define LUA_VERSION	"Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
#define LUA_RELEASE	LUA_VERSION "." LUA_VERSION_RELEASE

/******************************************************************************
* Lua 5.5 API declarations mirror lua.org's lua.h (MIT). The clx bridge in
* src/runtime/luaapi.cpp implements them on the clx runtime; symbols are
* prefixed clx_lua_* via the rename block above.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#endif
