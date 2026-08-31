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

//------------------ DeferredBlockScope: state emitBlock captures when a repeat body must
struct DeferredBlockScope {
    size_t prev_locals = 0;                    // locals.size() to restore after condition emission
    size_t prev_native_count = 0;              // native_numbers.size() to restore
    std::set<std::string_view> prev_hoisted;   // hoisted_locals to restore
    int close_braces = 0;                      // number of "}" to emit to close this body's scopes
    bool emit_close = false;                   // whether an outer "}" is owed (body emitted its own "{")
};

//------------------ LocalVar: tracks a local variable's name, boxed status, and has_sb flag
struct LocalVar {
    std::string_view name;
    std::string cpp_name;
    bool is_boxed;
    bool has_sb = false;
    LocalVar() = default;
    LocalVar(std::string_view n, bool boxed) : name(n), cpp_name(), is_boxed(boxed) {}
    LocalVar(std::string_view n, std::string cpp, bool boxed) : name(n), cpp_name(std::move(cpp)), is_boxed(boxed) {}
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
    bool is_local(std::string_view name, bool& out_is_boxed, std::string_view& out_cpp_name);

    //------------------ var_reassigned_non_int: checks if a variable receives any non-integer value in a block tree
    bool var_reassigned_non_int(std::string_view name, uint32_t block_idx);

    //------------------ emit_node: dispatches to the emitXxx method matching node's type
    void emit_node(uint32_t node_idx);

    //------------------ emit_native: emits an expression coerced to a raw C++ double
    void emit_native(uint32_t n_idx);

    //------------------ emit_condition: emits a boolean C++ expression for use in `if`/`while`
    void emit_condition(uint32_t c_idx);

    //------------------ emitXxx: emission logic for one AST NodeType, called from emit_node's dispatcher
    void emitIntrinsicCall(const ASTNode& node, uint32_t node_idx);
    void emitCallExpression(const ASTNode& node, uint32_t node_idx);
    void emitParenExpression(const ASTNode& node, uint32_t node_idx);
    void emitLabelStatement(const ASTNode& node, uint32_t node_idx);
    void emitGotoStatement(const ASTNode& node, uint32_t node_idx);
    void emitBlock(const ASTNode& node, uint32_t node_idx, DeferredBlockScope* defer = nullptr);
    void emitFunctionDef(const ASTNode& node, uint32_t node_idx);
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
