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
        , is_boxed(boxed)
    {
    }

    LocalVar(std::string_view n, std::string cpp, bool boxed)
        : name(n)
        , cpp_name(std::move(cpp))
        , is_boxed(boxed)
    {
    }
};

//------------------ lookup_builtin: maps "module.func" to C++ function name
const char* lookup_builtin(std::string_view module, std::string_view func);

//------------------ CodeEmitter: generates C++ source from the AST.
class CodeEmitter {
public:
    //------------------ CodeEmitter: constructs emitter for a given AST context, output file, and analysis results
    CodeEmitter(const ASTContext& context, const char* output_path, AnalysisState& analysis);

    //------------------ emit: generates C++ code for the AST rooted at root_node
    void emit(uint32_t root_node, std::string_view module_name);

private:
    const ASTContext& ctx;
    std::ofstream out;
    std::vector<LocalVar> locals;
    AnalysisState& state;

    //------------------ is_local: checks if name is a local and sets out_is_boxed
    bool is_local(std::string_view name, bool& out_is_boxed);

    //------------------ gc_cells_arg: initializer list of in-scope upvalue cells for GC rooting of escaped closures
    std::string gc_cells_arg(const std::string& exclude = "");
    bool is_local(std::string_view name, bool& out_is_boxed, std::string_view& out_cpp_name);

    //------------------ int_flag_expr: C++ bool expression for the runtime integer subtype of an expression
    std::string int_flag_expr(uint32_t expr_idx, int depth);
    //------------------ int_value_expr: C++ int64 expression for an expression proven integral by int_flag_expr
    std::string int_value_expr(uint32_t expr_idx, int depth);
    //------------------ fast_call_box_flag: C++ bool expression restoring the return subtype of a fast call
    std::string fast_call_box_flag(std::string_view fname, uint32_t first_arg, uint32_t arg_count);

    //------------------ box_native_identifier: boxes a native double local/param, restoring Int64 subtype via its runtime flag when present
    void box_native_identifier(std::string_view emit_name, std::string_view lua_name);

    //------------------ var_reassigned_non_int: checks if a variable receives any new non-integer value
    bool var_reassigned_non_int(std::string_view name, uint32_t block_idx, uint32_t exclude_value_node = 0xFFFFFFFF);

    //------------------ emit_node: dispatches to the emitXxx method matching node's type
    void emit_node(uint32_t node_idx);

    //------------------ emit_native: emits an expression coerced to a raw C++ double
    void emit_native(uint32_t n_idx);

    //------------------ emit_condition: emits a boolean C++ expression for use in `if`/`while`
    void emit_condition(uint32_t c_idx);

    //------------------ emitXxx: emission logic for one AST NodeType, called from emit_node's dispatcher
    void emitIntrinsicCall(const ASTNode& node, uint32_t node_idx);
    void emitCallExpression(const ASTNode& node, uint32_t node_idx);

    //------------------ impl_call: call expression for a direct-callable's impl, deref-ing heap holder cells
    std::string impl_call(std::string_view fname);
    //------------------ hoisted_table_ptr: hoisted header pointer for a
    // definitely-tabled local used as an index base, or "" when the table
    // node is not a hoisted local (callers emit the checked path instead)
    std::string hoisted_table_ptr(uint32_t table_idx);

    //------------------ local_root_snapshot_ok: true when every statement using a local
    // lowers to inline code, so a snapshot root can keep the local in a register
    bool local_root_snapshot_ok(std::string_view lua_name);

    //------------------ emit_local_root: pushes a shadow-stack root for a block local.
    // Locals that are never reassigned get a private snapshot slot so the local
    // itself is never addressed and can stay in a register on hot paths.
    void emit_local_root(std::string_view name);

    //------------------ emit_param_root: shadow-stack root for a parameter or a generic-for
    // variable, which are bound at their header rather than by a LocalDecl
    void emit_param_root(std::string_view lua_name, std::string_view cpp_name);

    //------------------ emit_loop_table_hoists: derive an LTable* once per loop
    // for definitely-table locals indexed in the body; added collects the names
    // registered so restore_loop_table_hoists can pop them after the loop
    void emit_loop_table_hoists(uint32_t body_idx, std::vector<std::string_view>& added);

    //------------------ restore_loop_table_hoists: removes names registered by emit_loop_table_hoists
    void restore_loop_table_hoists(const std::vector<std::string_view>& added);
    void emitParenExpression(const ASTNode& node, uint32_t node_idx);
    void emitLabelStatement(const ASTNode& node, uint32_t node_idx);
    void emitGotoStatement(const ASTNode& node, uint32_t node_idx);
    void emitBlock(const ASTNode& node, uint32_t node_idx, DeferredBlockScope* defer = nullptr);
    void emitFunctionDef(const ASTNode& node, uint32_t node_idx);
    void emit_native_impl_def(std::string_view name, uint32_t v_idx);
    //------------------ native call helpers: native_call_eligible is the pure

    bool native_call_eligible(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count);
    bool try_emit_native_call(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count, bool wrap_multi);
    void emitReturnStatement(const ASTNode& node, uint32_t node_idx);
    //------------------ emitAssignmentLike: handles GlobalDeclStatement, LocalDecl, and Assignment
    void emitAssignmentLike(const ASTNode& node, uint32_t node_idx);
    void emitDoStatement(const ASTNode& node, uint32_t node_idx);
    void emitUnaryOp(const ASTNode& node, uint32_t node_idx);
    void emitBinaryOp(const ASTNode& node, uint32_t node_idx);
    void emitTrueLiteral(const ASTNode& node, uint32_t node_idx);
    void emitFalseLiteral(const ASTNode& node, uint32_t node_idx);
    void emitNilLiteral(const ASTNode& node, uint32_t node_idx);
    void emitTableOp(int bin_op, uint32_t lhs_tbl, uint32_t lhs_key, uint32_t const_idx);
    void emitNumber(const ASTNode& node, uint32_t node_idx);
    void emitInteger(const ASTNode& node, uint32_t node_idx);
    void emitIdentifier(const ASTNode& node, uint32_t node_idx);
    void emitString(const ASTNode& node, uint32_t node_idx);
    void emitIfStatement(const ASTNode& node, uint32_t node_idx);
    void emitWhileStatement(const ASTNode& node, uint32_t node_idx);
    void emitRepeatStatement(const ASTNode& node, uint32_t node_idx);
    void emitForStatement(const ASTNode& node, uint32_t node_idx);
    void emitGenericForStatement(const ASTNode& node, uint32_t node_idx);
    void emitTableConstructor(const ASTNode& node, uint32_t node_idx);
    void emitTableAccess(const ASTNode& node, uint32_t node_idx);
    void emitVararg(const ASTNode& node, uint32_t node_idx);
    void emitBreakStatement(const ASTNode& node, uint32_t node_idx);
};

}

#endif
