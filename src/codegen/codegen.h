// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  codegen.h · CodeGen Header                 │
// └─────────────────────────────────────────────┘

#ifndef CODEGEN_H
#define CODEGEN_H

#include "../optimizer/analysis_state.h"
#include "../syntax/nodes.h"
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace clx {

//------------------ DeferredBlockScope: state emitBlock captures
struct DeferredBlockScope {
    size_t prev_locals = 0;
    size_t prev_native_count = 0;
    std::set<std::string_view> prev_hoisted;
    int close_braces = 0;
    bool emit_close = false;
};

//------------------ LocalVar: tracks a local variable's name, boxed status, and has_sb flag
struct LocalVar {
    std::string_view name;
    std::string cpp_name;
    bool is_boxed;
    bool has_sb = false;
    bool has_csnap = false;
    bool has_intf = false;
    bool has_ii = false;
    bool is_int_counter = false;
    LocalVar() = default;

    LocalVar(std::string_view n, bool boxed)
        : name(n)
        , cpp_name()
        , is_boxed(boxed) { }

    LocalVar(std::string_view n, std::string cpp, bool boxed)
        : name(n)
        , cpp_name(std::move(cpp))
        , is_boxed(boxed) { }
};

//------------------ lookup_builtin: maps "module.func" to its clx:: CFunc symbol name, else nullptr
const char *lookup_builtin(std::string_view module, std::string_view func);

//------------------ CodeEmitter: generates C++ source from the AST.
class CodeEmitter {
public:
    //------------------ CodeEmitter: constructs emitter for a given AST context, output file, and analysis results
    CodeEmitter(const ASTContext &context, const char *output_path, AnalysisState &analysis);

    //------------------ emit: generates C++ code for the AST rooted at root_node
    void emit(uint32_t root_node, std::string_view module_name);

private:
    const ASTContext &ctx;
    std::ostringstream out;
    std::string output_path;
    std::vector<LocalVar> locals;
    AnalysisState &state;

    //------------------ is_local: checks if name is a local and sets out_is_boxed
    bool is_local(std::string_view name, bool &out_is_boxed);

    //------------------ gc_cells_arg: initializer list of in-scope upvalue cells for GC rooting of escaped closures
    std::string gc_cells_arg(const std::string &exclude = "");

    //------------------ CallSiteEntry: one interned call-site descriptor, emitted into the per-TU
    //------------------ `_sites[]` table instead of a separate static object per site.
    struct CallSiteEntry {
        int line;
        bool is_method;
        bool name_short;
    };

    std::vector<CallSiteEntry> call_sites;
    std::map<uint64_t, uint32_t> call_site_ids;
    std::string call_site_table();
    bool is_local(std::string_view name, bool &out_is_boxed, std::string_view &out_cpp_name);

    //------------------ note_global_ref: records a global read for filtered std registration.
    //------------------ _G/_ENV reads set the dynamic-env fallback (full registration) instead.
    void note_global_ref(std::string_view name);

    //------------------ int_flag_expr: C++ bool expression for the runtime integer subtype of an expression
    std::string int_flag_expr(uint32_t expr_idx, int depth);
    //------------------ int_value_expr: C++ int64 expression for an expression proven integral by int_flag_expr
    std::string int_value_expr(uint32_t expr_idx, int depth);
    //------------------ fast_call_box_flag: C++ bool expression restoring the return subtype of a fast call
    std::string fast_call_box_flag(std::string_view fname, uint32_t first_arg, uint32_t arg_count);

    //------------------ box_native_identifier: boxes a native double local/param, restoring Int64 via its runtime flag.
    void box_native_identifier(std::string_view emit_name, std::string_view lua_name);

    //------------------ var_reassigned_non_int: checks if a variable receives any new non-integer value
    bool var_reassigned_non_int(std::string_view name, uint32_t block_idx, uint32_t exclude_value_node = 0xFFFFFFFF);

    //------------------ emit_call_site: emits one static CallSite descriptor and returns its address expression
    std::string emit_call_site(int line, bool is_method, bool name_short);

    //------------------ emit_node: dispatches to the emitXxx method matching node's type
    void emit_node(uint32_t node_idx);

    //------------------ emit_native: emits an expression coerced to a raw C++ double
    void emit_native(uint32_t n_idx);

    //------------------ emit_condition: emits a boolean C++ expression for use in `if`/`while`
    void emit_condition(uint32_t c_idx);

    //------------------ emitXxx: emission logic for one AST NodeType, called from emit_node's dispatcher
    void emitIntrinsicCall(const ASTNode &node, uint32_t node_idx);
    //------------------ emit_intrinsic_native_body: raw double-context emission of an IntrinsicCall
    //------------------ (alias call sites synthesize a temp node, so alias lowering shares one path)
    void emit_intrinsic_native_body(const ASTNode &node);
    void emitCallExpression(const ASTNode &node, uint32_t node_idx);

    //------------------ impl_call: call expression for a direct-callable's impl, deref-ing heap holder cells
    std::string impl_call(std::string_view fname);
    //------------------ hoisted_table_ptr: header pointer for a hoisted index-base local, else ""; callers emit the checked path.
    std::string hoisted_table_ptr(uint32_t table_idx);

    //------------------ try_emit_array_cmp: numeric fast path for two hoisted integer-indexed array reads; returns false to use the generic path.
    //------------------ want_bool (condition position) emits a bool lambda instead of an LValue one, so callers skip the box/unbox round trip.
    bool try_emit_array_cmp(int cmp_op, uint32_t l_idx, uint32_t r_idx, bool want_bool = false);

    //------------------ SwapIdiomMatch: a proven `local x = A[i]; A[j] = A[k]; A[l] = x` sequence eligible for fusion.
    struct SwapIdiomMatch {
        uint32_t temp_ident = 0xFFFFFFFF;
        uint32_t table_ident = 0xFFFFFFFF;
        uint32_t read_a = 0xFFFFFFFF;
        uint32_t read_b = 0xFFFFFFFF;
        uint32_t key_a = 0xFFFFFFFF;
        uint32_t key_b = 0xFFFFFFFF;
    };

    //------------------ try_match_swap_idiom: true when statements `pos`..`pos+2` of a block are the array swap idiom
    //------------------ on one hoisted table. Fusing them removes the boxed temp and its frame root.
    bool try_match_swap_idiom(uint32_t block_idx, uint32_t pos, SwapIdiomMatch &m);

    //------------------ emit_swap_idiom: raw two-slot exchange on a hoisted array; generic table_get/table_set fallback
    //------------------ when either index is outside the array part.
    void emit_swap_idiom(const SwapIdiomMatch &m, uint32_t tag);

    //------------------ CmpSwapMatch: an `if (A[i] </<=/>/>= A[j])` guard whose then-block is exactly the
    //------------------ swap idiom on the same two slots. Fusing them evaluates the keys once and proves
    //------------------ the bounds/tags once for both the compare and the exchange.
    struct CmpSwapMatch {
        uint32_t table_ident = 0xFFFFFFFF;
        uint32_t ord_l = 0xFFFFFFFF;
        uint32_t ord_r = 0xFFFFFFFF;
        uint32_t key_ol = 0xFFFFFFFF;
        uint32_t key_or = 0xFFFFFFFF;
        int cmp_op = 0;
        uint32_t cond_line = 0;
        uint32_t body_line = 0;
    };

    //------------------ try_match_cmp_swap: true for a fused compare+swap `if` (then-only shape checked
    //------------------ loosely — an else attaches to the fused block normally).
    bool try_match_cmp_swap(uint32_t if_idx, CmpSwapMatch &m);

    //------------------ emit_cmp_swap: one guard lattice feeding a native compare and, when true, a raw
    //------------------ exchange; generic compare + generic stores when unproven.
    void emit_cmp_swap(const CmpSwapMatch &m, uint32_t tag);

    //------------------ local_root_snapshot_ok: every use lowers inline, so a snapshot root can keep the local in a register.
    bool local_root_snapshot_ok(std::string_view lua_name);

    //------------------ emit_local_root: root a block local; never-reassigned locals use a snapshot slot and stay in registers.
    void emit_local_root(std::string_view name);

    //------------------ frame_root_exempt: this block local is in nogc_locals (never holds a GC object) — no frame slot.
    bool frame_root_exempt(std::string_view name);

    //------------------ emit_param_root: frame root for a parameter/generic-for variable (bound at header, not by LocalDecl).
    void emit_param_root(std::string_view lua_name, std::string_view cpp_name);

    //------------------ emit_loop_table_hoists: hoist an LTable* per loop for definitely-tabled locals; added collects names to restore.
    void emit_loop_table_hoists(uint32_t body_idx, std::vector<std::string_view> &added);

    //------------------ restore_loop_table_hoists: removes names registered by emit_loop_table_hoists
    void restore_loop_table_hoists(const std::vector<std::string_view> &added);
    void emitParenExpression(const ASTNode &node, uint32_t node_idx);
    void emitLabelStatement(const ASTNode &node, uint32_t node_idx);
    void emitGotoStatement(const ASTNode &node, uint32_t node_idx);
    void emitBlock(const ASTNode &node, uint32_t node_idx, DeferredBlockScope *defer = nullptr);
    void emitFunctionDef(const ASTNode &node, uint32_t node_idx);
    void emit_native_impl_def(std::string_view name, uint32_t v_idx);
    //------------------ emit_fast_variant: emits the `_fast_<name>_impl` body and `_fast_<name>` wrapper
    void emit_fast_variant(std::string_view name, uint32_t v_idx);
    //------------------ emit_dbl_impl_def: triple-bound (double, int-flag, int-shadow) overload of a
    //------------------ native-direct single-return function; call sites with double-ready args bypass boxing.
    void emit_dbl_impl_def(std::string_view name, uint32_t v_idx);
    //------------------ emit_multi_impl_def: tuple-struct native overload for a multi-double function
    void emit_multi_impl_def(std::string_view name, uint32_t v_idx, uint32_t tuple_n);
    //------------------ native call helpers: native_call_eligible is the pure
    //------------------ args_need_sb_decl: an argument references a string-builder local with no
    //------------------ enclosing builder in scope — only the generic call path can declare it on
    //------------------ demand, so native/dbl emission must decline these calls.
    bool args_need_sb_decl(uint32_t first_arg, uint32_t arg_count);

    bool native_call_eligible(
        uint32_t target_idx, uint32_t first_arg, uint32_t arg_count, bool allow_trailing_single = false, int depth = 0);
    //------------------ trailing_single_call: trailing call arg provably yielding exactly 1 value
    //------------------ (native-direct callee, exact arity) — needs no multi-value expansion.
    uint32_t trailing_single_call(uint32_t first_arg, uint32_t arg_count, int depth = 0);
    bool try_emit_native_call(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count, bool wrap_multi,
        bool allow_trailing_single = false);
    //------------------ unboxed_arg_ok: arg is a literal, exact-flag native local/param/counter, or
    //------------------ arithmetic over such — its double, int-flag and int-shadow are all exact, so a
    //------------------ triple-bound call passes the identical value the boxed path would unbox to.
    bool unboxed_arg_ok(uint32_t arg_idx, int depth = 0);
    //------------------ dbl_call_eligible: native-direct callee with a triple overload emitted, exact
    //------------------ arity, and every arg double-ready — the call bypasses LValue boxing entirely.
    bool dbl_call_eligible(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count);
    //------------------ dbl_self_ref: target names the triple body being emitted — the wrapper
    //------------------ name is unusable there (it would capture itself); callers fall back to boxed.
    bool dbl_self_ref(uint32_t target_idx);
    bool try_emit_dbl_call(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count);
    //------------------ multi call helpers: exact-arity tuple calls bypass the closure + MultiValue path
    bool multi_call_eligible(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count, uint32_t &out_n);
    void emit_multi_call(std::string_view fname, uint32_t first_arg, uint32_t arg_count);
    void emitReturnStatement(const ASTNode &node, uint32_t node_idx);
    //------------------ emitAssignmentLike: handles GlobalDeclStatement, LocalDecl, and Assignment
    void emitAssignmentLike(const ASTNode &node, uint32_t node_idx);
    void emitDoStatement(const ASTNode &node, uint32_t node_idx);
    void emitUnaryOp(const ASTNode &node, uint32_t node_idx);
    void emitBinaryOp(const ASTNode &node, uint32_t node_idx);
    void emitTrueLiteral(const ASTNode &node, uint32_t node_idx);
    void emitFalseLiteral(const ASTNode &node, uint32_t node_idx);
    void emitNilLiteral(const ASTNode &node, uint32_t node_idx);
    void emitTableOp(int bin_op, uint32_t lhs_tbl, uint32_t lhs_key, uint32_t const_idx, bool nil_ok);
    void emitNumber(const ASTNode &node, uint32_t node_idx);
    void emitInteger(const ASTNode &node, uint32_t node_idx);
    void emitIdentifier(const ASTNode &node, uint32_t node_idx);
    void emitString(const ASTNode &node, uint32_t node_idx);
    void emitIfStatement(const ASTNode &node, uint32_t node_idx);
    void emitWhileStatement(const ASTNode &node, uint32_t node_idx);
    void emitRepeatStatement(const ASTNode &node, uint32_t node_idx);
    void emitForStatement(const ASTNode &node, uint32_t node_idx);
    void emitGenericForStatement(const ASTNode &node, uint32_t node_idx);
    void emitTableConstructor(const ASTNode &node, uint32_t node_idx);
    void emitTableAccess(const ASTNode &node, uint32_t node_idx);
    void emitVararg(const ASTNode &node, uint32_t node_idx);
    void emitBreakStatement(const ASTNode &node, uint32_t node_idx);
};

}

#endif
