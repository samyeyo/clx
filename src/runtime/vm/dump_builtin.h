// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  dump_builtin.h · string.dump via the VM    │
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_DUMP_BUILTIN_H
#define CLX_VM_DUMP_BUILTIN_H

#include <clx_runtime.h>

namespace clx {

//------------------ clx_string_dump_dynamic - string.dump under --dynamic: VM bytecode strings, native closures as boxes
MultiValue clx_string_dump_dynamic(LState *L, const LValue *args, size_t count);

}

#endif
