// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  dump_builtin.cpp · string.dump via the VM  │
// └─────────────────────────────────────────────┘

#include "dynamic_vm.h"
#include "vm_convert.h"
#include "vm_function_object.h"
#include "../internal.h"

#include <clx.h>
#include <clx_runtime.h>

#include <string>

namespace clx {

//------------------ dump_writer - lua_Writer appending into a std::string
static int dump_writer(lua_State *, const void *p, size_t sz, void *ud) {
    static_cast<std::string *>(ud)->append(static_cast<const char *>(p), sz);
    return 0;
}

//------------------ clx_string_dump_dynamic: string.dump under --dynamic — VM bytecode strings, native closures as callable boxes
MultiValue clx_string_dump_dynamic(LState *L, const LValue *args, size_t count) {
    if (count < 1 || args[0].type != Function || args[0].as_pointer() == nullptr)
        throw_runtime_error(bad_arg_msg(L, 1, "string.dump", "Lua function expected"));
    const LValue &fn = args[0];
    DynamicVM *vm = DynamicVM::acquire(L);
    if (!vm)
        throw_runtime_error("string.dump not supported");
    lua_State *luaL = vm->lua_L();
    int strip = (count > 1) ? (to_boolean(args[1]) ? 1 : 0) : 0;

    LCFunction *f = static_cast<LCFunction *>(fn.as_pointer());
    if (f->flags & LFLAG_VM_PROXY) {
        VMFunction *vf = static_cast<VMFunction *>(f);
        lua_rawgeti(luaL, LUA_REGISTRYINDEX, vf->registry_ref);
        if (lua_isfunction(luaL, -1) && !lua_iscfunction(luaL, -1)) {
            std::string out;
            int status = lua_dump(luaL, dump_writer, &out, strip);
            lua_pop(luaL, 1);
            if (status != 0)
                throw_runtime_error("unable to dump given function");
            return MultiValue(LValue(L->intern_string(out.data(), out.size())));
        }
        lua_pop(luaL, 1);
        return MultiValue(fn);
    }

    clx_to_vm_value_(L, luaL, fn);
    if (!lua_isfunction(luaL, -1)) {
        lua_pop(luaL, 1);
        throw_runtime_error(bad_arg_msg(L, 1, "string.dump", "Lua function expected"));
    }
    LValue boxed = VMFunction::wrap(L, luaL, -1);
    lua_pop(luaL, 1);
    return MultiValue(boxed);
}

}
