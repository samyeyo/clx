// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  passes.h · Optimizer pass interface        │
// └─────────────────────────────────────────────┘

#ifndef OPTIMIZER_PASSES_H
#define OPTIMIZER_PASSES_H

#include "analysis_state.h"
#include "optimizer.h"
#include "../syntax/nodes.h"
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clx {

//------------------ PassScratch: run()-scoped working state shared between sequential passes
struct PassScratch {
    std::set<std::string_view> known_numbers;
    std::set<std::string_view> disqualified;
    std::set<std::string_view> for_vars;
    std::map<std::string, uint32_t> array_bounds;
    std::map<std::string, uint32_t> loop_limits;
    std::map<std::string, bool> loop_limit_conflicts;
    std::vector<std::pair<std::string_view, uint32_t>> func_defs;
    std::map<uint32_t, std::string_view> call_to_func;
    std::set<std::string_view> disqualified_arrays;
    std::set<std::string_view> empty_array_candidates;
    std::set<std::string_view> param_names;
};

//------------------ get_ast_string: convert AST node to string for analysis
std::string get_ast_string(const ASTContext &ctx, uint32_t node_idx);

//------------------ function_returns_native: check if function always returns native number
bool function_returns_native(const ASTContext &ctx, const AnalysisState &state, uint32_t func_idx,
    std::string_view self_name, const std::set<std::string_view> *known_numbers);

//------------------ is_literal_number: check if node is a literal integer or number
bool is_literal_number(const ASTContext &ctx, uint32_t node_idx, double &out_d, int64_t &out_i, bool &out_is_int);

//------------------ traverse_node: depth-first AST walk invoking cb(node_idx) on every reachable node
template<typename Cb> void traverse_node(const ASTContext &ctx, uint32_t node_idx, Cb &cb) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return;
    cb(node_idx);
    const auto &n = ctx.nodes[node_idx];
    if (n.type == NodeType::Block) {
        for (uint32_t i = 0; i < n.as.block.count; ++i)
            traverse_node(ctx, ctx.block_statements[n.as.block.first_statement + i], cb);
    } else if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement
        || n.type == NodeType::Assignment) {
        uint32_t v_count = (n.type == NodeType::LocalDecl)
            ? n.as.local_decl.value_count
            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count : n.as.assign.value_count);
        uint32_t f_value = (n.type == NodeType::LocalDecl)
            ? n.as.local_decl.first_value
            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value : n.as.assign.first_value);
        uint32_t id_count = (n.type == NodeType::LocalDecl)
            ? n.as.local_decl.ident_count
            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.ident_count : n.as.assign.target_count);
        uint32_t f_ident = (n.type == NodeType::LocalDecl)
            ? n.as.local_decl.first_ident
            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_ident : n.as.assign.first_target);
        for (uint32_t i = 0; i < id_count; ++i)
            traverse_node(ctx, ctx.block_statements[f_ident + i], cb);
        for (uint32_t i = 0; i < v_count; ++i)
            traverse_node(ctx, ctx.block_statements[f_value + i], cb);
    } else if (n.type == NodeType::BinaryOp) {
        traverse_node(ctx, n.as.bin_op.left, cb);
        traverse_node(ctx, n.as.bin_op.right, cb);
    } else if (n.type == NodeType::UnaryOp) {
        traverse_node(ctx, n.as.unary_op.expr, cb);
    } else if (n.type == NodeType::CallExpression) {
        traverse_node(ctx, n.as.call_expr.target, cb);
        for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i)
            traverse_node(ctx, ctx.block_statements[n.as.call_expr.first_arg + i], cb);
    } else if (n.type == NodeType::IntrinsicCall) {
        for (uint32_t i = 0; i < n.as.intrinsic_call.arg_count; ++i)
            traverse_node(ctx, ctx.block_statements[n.as.intrinsic_call.first_arg + i], cb);
    } else if (n.type == NodeType::IfStatement) {
        traverse_node(ctx, n.as.if_stmt.condition, cb);
        traverse_node(ctx, n.as.if_stmt.then_block, cb);
        traverse_node(ctx, n.as.if_stmt.else_block, cb);
    } else if (n.type == NodeType::WhileStatement) {
        traverse_node(ctx, n.as.while_stmt.condition, cb);
        traverse_node(ctx, n.as.while_stmt.body_block, cb);
    } else if (n.type == NodeType::RepeatStatement) {
        traverse_node(ctx, n.as.repeat_stmt.body_block, cb);
        traverse_node(ctx, n.as.repeat_stmt.condition, cb);
    } else if (n.type == NodeType::ForStatement) {
        traverse_node(ctx, n.as.for_stmt.start_expr, cb);
        traverse_node(ctx, n.as.for_stmt.limit_expr, cb);
        traverse_node(ctx, n.as.for_stmt.step_expr, cb);
        traverse_node(ctx, n.as.for_stmt.body_block, cb);
    } else if (n.type == NodeType::GenericForStatement) {
        for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
            traverse_node(ctx, ctx.block_statements[n.as.generic_for.first_iter + i], cb);
        traverse_node(ctx, n.as.generic_for.body_block, cb);
    } else if (n.type == NodeType::DoStatement) {
        traverse_node(ctx, n.as.do_stmt.body_block, cb);
    } else if (n.type == NodeType::TableConstructor) {
        for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
            traverse_node(ctx, ctx.block_statements[n.as.table_cons.first_item + i * 2], cb);
            traverse_node(ctx, ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1], cb);
        }
    } else if (n.type == NodeType::TableAccess) {
        traverse_node(ctx, n.as.table_access.table, cb);
        traverse_node(ctx, n.as.table_access.key, cb);
    } else if (n.type == NodeType::ReturnStatement) {
        for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
            traverse_node(ctx, ctx.block_statements[n.as.return_stmt.first_value + i], cb);
    } else if (n.type == NodeType::FunctionDef) {
        traverse_node(ctx, n.as.func_def.body_block, cb);
    } else if (n.type == NodeType::ParenExpression) {
        traverse_node(ctx, n.as.paren_expr.expr, cb);
    }
}

//------------------ pass_node_func_owner: map every node to its innermost enclosing FunctionDef
void pass_node_func_owner(const ASTContext &ctx, AnalysisState &state, uint32_t root_node);

//------------------ pass_goto_labels: resolve goto/label targets into state.goto_targets
void pass_goto_labels(const ASTContext &ctx, AnalysisState &state, uint32_t root_node);

//------------------ pass_name_scans: reassigned/assigned targets and numeric-for counter names
void pass_name_scans(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_constants_and_bounds: constant upvalues, global constants, loop/array bounds
void pass_constants_and_bounds(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_callables: function index, direct/fast-callable qualification, native-direct funcs
void pass_callables(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_pure_arrays: pure numeric array detection and disqualification
void pass_pure_arrays(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_array_safety: safety passes over pure-array promotions
void pass_array_safety(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_table_fields: numeric table field typing and zero-index proof
void pass_table_fields(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_numeric_params: numeric parameter detection and native_numbers finalization
void pass_numeric_params(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_arena: escaping variables and arena-safe table sizing
void pass_arena(const ASTContext &ctx, AnalysisState &state, uint32_t root_node);

//------------------ pass_int_returns_masks: integer-returning functions and int-preserving masks
void pass_int_returns_masks(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node);

//------------------ pass_local_hygiene: goto-crossed locals, definite-table locals, dead literal tables
void pass_local_hygiene(const ASTContext &ctx, AnalysisState &state, uint32_t root_node);

}

#endif
