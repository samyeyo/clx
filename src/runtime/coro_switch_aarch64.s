// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  coro_switch_aarch64.s                      │
// │  ARM64 coroutine context switching primitives│
// └─────────────────────────────────────────────┘

.section __TEXT,__text
.align 2

// CoroutineContext struct offsets
.equ CTX_X19,    0
.equ CTX_X20,    8
.equ CTX_X21,    16
.equ CTX_X22,    24
.equ CTX_X23,    32
.equ CTX_X24,    40
.equ CTX_X25,    48
.equ CTX_X26,    56
.equ CTX_X27,    64
.equ CTX_X28,    72
.equ CTX_FP,     80    // x29
.equ CTX_LR,     88    // x30
.equ CTX_SP,     96
.equ CTX_D8,     104
.equ CTX_D9,     112
.equ CTX_D10,    120
.equ CTX_D11,    128
.equ CTX_D12,    136
.equ CTX_D13,    144
.equ CTX_D14,    152
.equ CTX_D15,    160
.equ CTX_SIZE,   168


//------------------ clx_coro_save(CoroutineContext* ctx)
// Save all callee-saved registers (x19-x30, sp, d8-d15) into *ctx.
// Returns normally — the captured return address means a later
// switch into this context resumes after the original call.
.globl _clx_coro_save
_clx_coro_save:
    stp x19, x20, [x0, #CTX_X19]
    stp x21, x22, [x0, #CTX_X21]
    stp x23, x24, [x0, #CTX_X23]
    stp x25, x26, [x0, #CTX_X25]
    stp x27, x28, [x0, #CTX_X27]
    stp x29, x30, [x0, #CTX_FP]
    mov  x2, sp
    str  x2, [x0, #CTX_SP]
    stp d8,  d9,  [x0, #CTX_D8]
    stp d10, d11, [x0, #CTX_D10]
    stp d12, d13, [x0, #CTX_D12]
    stp d14, d15, [x0, #CTX_D14]
    ret


//------------------ clx_coro_switch(CoroutineContext* from, CoroutineContext* to)
// Save current callee-saved registers to *from, restore from *to,
// then jump to the restored return address (context switch).
.globl _clx_coro_switch
_clx_coro_switch:
    // Save current context to [x0]
    stp x19, x20, [x0, #CTX_X19]
    stp x21, x22, [x0, #CTX_X21]
    stp x23, x24, [x0, #CTX_X23]
    stp x25, x26, [x0, #CTX_X25]
    stp x27, x28, [x0, #CTX_X27]
    stp x29, x30, [x0, #CTX_FP]
    mov  x2, sp
    str  x2, [x0, #CTX_SP]
    stp d8,  d9,  [x0, #CTX_D8]
    stp d10, d11, [x0, #CTX_D10]
    stp d12, d13, [x0, #CTX_D12]
    stp d14, d15, [x0, #CTX_D14]

    // Restore target context from [x1]
    ldp x19, x20, [x1, #CTX_X19]
    ldp x21, x22, [x1, #CTX_X21]
    ldp x23, x24, [x1, #CTX_X23]
    ldp x25, x26, [x1, #CTX_X25]
    ldp x27, x28, [x1, #CTX_X27]
    ldp x29, x30, [x1, #CTX_FP]
    ldr x2,  [x1, #CTX_SP]
    mov sp, x2
    ldp d8,  d9,  [x1, #CTX_D8]
    ldp d10, d11, [x1, #CTX_D10]
    ldp d12, d13, [x1, #CTX_D12]
    ldp d14, d15, [x1, #CTX_D14]

    ret


//------------------ clx_coro_init(CoroutineContext* ctx, void* stack_top, void* entry)
// Initialise a fresh context for a new coroutine.
// Sets sp = stack_top, lr = entry, zeros all other callee-saved slots.
.globl _clx_coro_init
_clx_coro_init:
    str  x1, [x0, #CTX_SP]
    str  x2, [x0, #CTX_LR]

    stp xzr, xzr, [x0, #CTX_X19]
    stp xzr, xzr, [x0, #CTX_X21]
    stp xzr, xzr, [x0, #CTX_X23]
    stp xzr, xzr, [x0, #CTX_X25]
    stp xzr, xzr, [x0, #CTX_X27]
    str xzr,      [x0, #CTX_FP]
    stp xzr, xzr, [x0, #CTX_D8]
    stp xzr, xzr, [x0, #CTX_D10]
    stp xzr, xzr, [x0, #CTX_D12]
    stp xzr, xzr, [x0, #CTX_D14]
    ret
