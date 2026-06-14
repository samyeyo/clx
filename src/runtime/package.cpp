// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  package.cpp · Package/Module System        │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <cstring>
#include <string>

namespace clx {


//------------------ pack_require: implements Lua's require() function
static MultiValue pack_require(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'require' (string expected, got no value)");
    
    const char* modname = check_string(L, args[0]);
    LValue loaded_key = LValue(L->intern_string("loaded"));
    LValue preload_key = LValue(L->intern_string("preload"));
    
    LValue pack_val = get_global(L, "package");
    if (pack_val.type() != LType::Table)
        throw_runtime_error("global 'package' is not a table");
        
    LTable* pack = static_cast<LTable*>(pack_val.as_pointer());
    
    LValue* loaded_ptr = pack->gettable(loaded_key);
    if (!loaded_ptr || loaded_ptr->type() != LType::Table)
        throw_runtime_error("'package.loaded' is not a table");
    LTable* loaded = static_cast<LTable*>(loaded_ptr->as_pointer());
    
    LValue mod_key = LValue(L->intern_string(modname));
    LValue* already_loaded = loaded->gettable(mod_key);
    if (already_loaded && already_loaded->type() != LType::Nil) {
        return MultiValue(*already_loaded);
    }
    
    LValue* preload_ptr = pack->gettable(preload_key);
    if (preload_ptr && preload_ptr->type() == LType::Table) {
        LTable* preload = static_cast<LTable*>(preload_ptr->as_pointer());
        LValue* loader = preload->gettable(mod_key);
        if (loader && loader->type() == LType::Function) {
            LValue loader_args[2] = { mod_key, *loader };
            L->shadow_stack[L->shadow_top++] = &loader_args[0];
            L->shadow_stack[L->shadow_top++] = &loader_args[1];
            MultiValue res = call_function(L, *loader, loader_args, 1, __FILE__, __LINE__);
            L->shadow_top -= 2;
            
            LValue result = (res.count > 0) ? res[0] : LValue(true);
            if (result.type() == LType::Nil) result = LValue(true);
            loaded->settable(mod_key, result);
            return MultiValue(result);
        }
    }
    
    char err_buf[256];
    std::snprintf(err_buf, sizeof(err_buf), "module '%s' not found", modname);
    throw LRuntimeException(LValue(L->intern_string(err_buf)));
}


//------------------ luastd_package: registers the package library (package table + require)
void luastd_package(LState* L) {
    LValue pack_tbl = L->create_table();
    LTable* pack = static_cast<LTable*>(pack_tbl.as_pointer());
    
    LValue loaded_tbl = L->create_table();
    LValue preload_tbl = L->create_table();
    
    pack->settable(LValue(L->intern_string("loaded")), loaded_tbl);
    pack->settable(LValue(L->intern_string("preload")), preload_tbl);
    
    pack->settable(LValue(L->intern_string("config")), LValue(L->intern_string("/\n;\n?\n!\n-\n")));

    set_global(L, "package", pack_tbl);
    
    LValue require_func = L->create_closure(pack_require);
    set_global(L, "require", require_func);
    
    static_cast<LTable*>(loaded_tbl.as_pointer())->settable(LValue(L->intern_string("package")), pack_tbl);
}

//------------------ clx_register_static_preload: registers a static preload loader for a module
void clx_register_static_preload(LState* L, const char* name, LValue (*open_func)(LState*)) {
    LValue pack_val = get_global(L, "package");
    if (pack_val.type() != LType::Table) return;
    
    LTable* pack = static_cast<LTable*>(pack_val.as_pointer());
    LValue* preload_ptr = pack->gettable(LValue(L->intern_string("preload")));
    if (!preload_ptr || preload_ptr->type() != LType::Table) return;
    
    LTable* preload = static_cast<LTable*>(preload_ptr->as_pointer());
    
    LValue func_obj = LValue(L->create_closure([open_func](LState* L, const LValue* args, size_t count) -> MultiValue {
        return MultiValue(open_func(L));
    }));
    
    preload->settable(LValue(L->intern_string(name)), func_obj);
}

}