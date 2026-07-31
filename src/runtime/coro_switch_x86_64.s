// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  coro_switch_x86_64.s                       │
// │  x86_64 SysV coroutine context switching    │
// └─────────────────────────────────────────────┘

    .text

// CoroutineContext struct offsets (SysV ABI: callee-saved = rbx,rbp,r12-r15)
// rsp points to the return address on the stack — ret resumes the caller.
.equ CTX_RBX,    0
.equ CTX_RBP,    8
.equ CTX_R12,    16
.equ CTX_R13,    24
.equ CTX_R14,    32
.equ CTX_R15,    40
.equ CTX_RSP,    48
.equ CTX_SIZE,   56


//------------------ clx_coro_save(CoroutineContext* ctx)
// Save all callee-saved registers into *ctx.
// Returns normally — rsp (pointing at the return address) is saved,
// so a later switch resumes after the original call.
    .globl clx_coro_save
    .type  clx_coro_save, @function
clx_coro_save:
    movq %rbx, CTX_RBX(%rdi)
    movq %rbp, CTX_RBP(%rdi)
    movq %r12, CTX_R12(%rdi)
    movq %r13, CTX_R13(%rdi)
    movq %r14, CTX_R14(%rdi)
    movq %r15, CTX_R15(%rdi)
    movq %rsp, CTX_RSP(%rdi)
    ret


//------------------ clx_coro_switch(CoroutineContext* from, CoroutineContext* to)
// Save current callee-saved registers to *from, restore from *to,
// then ret to the restored return address (context switch).
    .globl clx_coro_switch
    .type  clx_coro_switch, @function
clx_coro_switch:
    // Save current context to [rdi]
    movq %rbx, CTX_RBX(%rdi)
    movq %rbp, CTX_RBP(%rdi)
    movq %r12, CTX_R12(%rdi)
    movq %r13, CTX_R13(%rdi)
    movq %r14, CTX_R14(%rdi)
    movq %r15, CTX_R15(%rdi)
    movq %rsp, CTX_RSP(%rdi)

    // Restore target context from [rsi]
    movq CTX_R15(%rsi), %r15
    movq CTX_R14(%rsi), %r14
    movq CTX_R13(%rsi), %r13
    movq CTX_R12(%rsi), %r12
    movq CTX_RBP(%rsi), %rbp
    movq CTX_RBX(%rsi), %rbx
    movq CTX_RSP(%rsi), %rsp
    ret


//------------------ clx_coro_init(CoroutineContext* ctx, void* stack_top, void* entry)
// Initialise a fresh context for a new coroutine.
// Sets rsp so that ret jumps to entry. Zeros all callee-saved slots.
    .globl clx_coro_init
    .type  clx_coro_init, @function
clx_coro_init:
    // Zero callee-saved registers
    movq $0, CTX_RBX(%rdi)
    movq $0, CTX_RBP(%rdi)
    movq $0, CTX_R12(%rdi)
    movq $0, CTX_R13(%rdi)
    movq $0, CTX_R14(%rdi)
    movq $0, CTX_R15(%rdi)

    // Set up stack: rsp = stack_top - 16, [rsp] = entry
    // so ret pops entry into rip, leaving rsp = stack_top - 8 (8 mod 16, ABI-correct).
    leaq -16(%rsi), %rax
    movq %rdx, (%rax)
    movq %rax, CTX_RSP(%rdi)
    ret

    .section .note.GNU-stack,"",@progbits
