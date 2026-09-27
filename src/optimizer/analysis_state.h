// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  analysis_state.h · Shared Analysis State   │
// └─────────────────────────────────────────────┘

#ifndef ANALYSIS_STATE_H
#define ANALYSIS_STATE_H

#include "../syntax/nodes.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace clx {

struct AnalysisState {
    //------------------ Whole-program facts discovered by the optimizer
    std::vector<std::string_view> native_numbers;
    std::vector<std::string_view> string_pool;
    std::unordered_map<std::string_view, size_t> string_pool_index;
    std::set<std::string_view> native_return_funcs;
    std::map<uint32_t, std::set<std::string>> param_numbers;
    std::vector<std::string> param_names;
    std::map<uint32_t, uint32_t> table_presize;
    std::map<std::string_view, double> global_constants;
    std::set<uint32_t> bce_safe_nodes;
    std::set<std::string_view> pure_numeric_arrays;
    std::set<std::string_view> tables_with_dynamic_length;
    std::map<std::string_view, size_t> known_table_lengths;
    std::map<std::string_view, std::set<uint32_t>> pure_numeric_func_params;
    std::map<uint32_t, uint32_t> node_func_owner;
    std::set<std::string, std::less<>> native_integers;
    std::set<std::string_view> int_returning_funcs;
    std::map<std::string_view, uint32_t> int_preserving_masks;
    std::set<std::string, std::less<>> int_typed_locals;
    std::set<std::string, std::less<>> table_typed_locals;

    //------------------ Arena analysis data
    std::set<std::string_view> escaping_vars;
    std::set<uint32_t> arena_safe_table_nodes;
    std::map<uint32_t, uint32_t> arena_table_sizes;

    //------------------ Per-function analysis data
    std::map<std::pair<uint32_t, std::string_view>, std::set<std::string_view>> numeric_table_fields;
    //------------------ zero_index_tables: fresh {__index=0}-metatable locals; fused t[k]=t[k]<op>const skips __index on miss.
    std::set<std::pair<uint32_t, std::string_view>> zero_index_tables;
    //------------------ dead_stmts: single-occurrence literal-only table decls, never observable; codegen drops them.
    std::set<uint32_t> dead_stmts;
    std::set<std::string_view> direct_callables;
    std::set<std::string_view> fast_callables;
    std::set<std::string_view> native_direct_funcs;
    std::map<std::string_view, uint32_t> func_param_counts;
    std::map<std::string_view, std::vector<bool>> func_param_native;
    std::set<std::string_view> reassigned_vars;
    std::set<std::string_view> assigned_targets;
    std::set<std::string_view> int_numeric_arrays;
    //------------------ for_counter_names: numeric-for loop counter identifiers. Codegen emits
    std::set<std::string_view> for_counter_names;
    //------------------ for_counter_int: numeric-for loop counter names holding integer values
    std::set<std::string_view> for_counter_int;
    std::set<std::string_view> constant_upvalues;
    std::set<std::string_view> string_builders;
    std::set<std::string_view> global_string_builders;
    std::set<std::string_view> module_string_builders;
    std::map<uint32_t, uint32_t> goto_targets;
    std::set<std::string_view> hoisted_locals;
    std::unordered_map<uint32_t, std::string> hoisted_lookups;
    std::unordered_map<std::string, std::string> hoisted_cfuncs;
    std::unordered_map<std::string, std::string> builtin_aliases;
    std::vector<std::string> const_snapshot_cells;

    //------------------ Codegen emission state (mutated as CodeEmitter walks the tree)
    bool skip_block_braces = false;
    bool emit_raw_lambda = false;
    bool emit_fast_lambda = false;
    bool emit_native_impl = false;
    bool in_function_def = false;
    bool in_fast_function = false;
    bool expect_multivalue = false;
    bool in_native_impl = false;
    std::string_view current_native_func;
    std::set<std::string_view> native_emitted;
    std::unordered_map<std::string_view, std::string> hoisted_tables;
    std::string_view current_fast_func;
    std::string ref_capture;
    std::string raw_lambda_cell;
    std::set<std::string_view> holder_cell_callables;
    std::map<std::pair<uint32_t, std::string_view>, bool> register_friendly_locals;
    uint32_t current_func_body = 0xFFFFFFFF;
    uint32_t current_arena_func = 0xFFFFFFFF;
    uint32_t current_func_idx = 0xFFFFFFFF;
};

//------------------ is_purely_integer_expr: returns true if a node always evaluates to an integer
inline bool is_purely_integer_expr(const ASTContext& ctx, const AnalysisState& state, uint32_t node_idx)
{
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto& n = ctx.nodes[node_idx];
    if (n.type == NodeType::Integer)
        return true;
    if (n.type == NodeType::Identifier) {
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        return state.native_integers.count(nm) > 0;
    }
    if (n.type == NodeType::BinaryOp) {
        int bop = n.as.bin_op.op;
        bool is_int_binop = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
            || bop == static_cast<int>(BinaryOp::Mod) || bop == static_cast<int>(BinaryOp::And)
            || bop == static_cast<int>(BinaryOp::Or) || bop == static_cast<int>(BinaryOp::FloorDiv)
            || bop == static_cast<int>(BinaryOp::BitAnd) || bop == static_cast<int>(BinaryOp::BitOr)
            || bop == static_cast<int>(BinaryOp::BitXor) || bop == static_cast<int>(BinaryOp::Shl)
            || bop == static_cast<int>(BinaryOp::Shr);
        if (is_int_binop)
            return is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                && is_purely_integer_expr(ctx, state, n.as.bin_op.right);
    }
    if (n.type == NodeType::UnaryOp
        && (n.as.unary_op.op == static_cast<int>(UnaryOp::Minus)
            || n.as.unary_op.op == static_cast<int>(UnaryOp::BNot)))
        return is_purely_integer_expr(ctx, state, n.as.unary_op.expr);
    if (n.type == NodeType::ParenExpression)
        return is_purely_integer_expr(ctx, state, n.as.paren_expr.expr);
    return false;
}

//------------------ reassigned_with_non_int: true if the variable is assigned a value that is not

inline bool reassigned_with_non_int(
    const ASTContext& ctx, const AnalysisState& state, std::string_view nm, uint32_t decl_idx)
{
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto& n = ctx.nodes[ni];
        if (n.type != NodeType::Assignment)
            continue;
        for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti) {
            uint32_t tgt = ctx.block_statements[n.as.assign.first_target + ti];
            if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier)
                continue;
            std::string_view tn(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (tn != nm || ni == decl_idx)
                continue;
            uint32_t vi
                = (ti < n.as.assign.value_count) ? ctx.block_statements[n.as.assign.first_value + ti] : 0xFFFFFFFF;
            if (!is_purely_integer_expr(ctx, state, vi))
                return true;
        }
    }
    return false;
}

//------------------ is_integer_typed_expr: stricter than is_purely_integer_expr
inline bool is_integer_typed_expr(const ASTContext& ctx, const AnalysisState& state, uint32_t node_idx)
{
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto& n = ctx.nodes[node_idx];
    if (n.type == NodeType::Integer)
        return true;
    if (n.type == NodeType::Identifier) {
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        return state.native_integers.count(nm) > 0;
    }
    if (n.type == NodeType::BinaryOp) {
        int bop = n.as.bin_op.op;
        bool is_int_binop = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
            || bop == static_cast<int>(BinaryOp::Mod) || bop == static_cast<int>(BinaryOp::And)
            || bop == static_cast<int>(BinaryOp::Or) || bop == static_cast<int>(BinaryOp::FloorDiv)
            || bop == static_cast<int>(BinaryOp::BitAnd) || bop == static_cast<int>(BinaryOp::BitOr)
            || bop == static_cast<int>(BinaryOp::BitXor) || bop == static_cast<int>(BinaryOp::Shl)
            || bop == static_cast<int>(BinaryOp::Shr);
        if (is_int_binop)
            return is_integer_typed_expr(ctx, state, n.as.bin_op.left)
                && is_integer_typed_expr(ctx, state, n.as.bin_op.right);
    }
    if (n.type == NodeType::UnaryOp
        && (n.as.unary_op.op == static_cast<int>(UnaryOp::Minus)
            || n.as.unary_op.op == static_cast<int>(UnaryOp::BNot)))
        return is_integer_typed_expr(ctx, state, n.as.unary_op.expr);
    if (n.type == NodeType::ParenExpression)
        return is_integer_typed_expr(ctx, state, n.as.paren_expr.expr);
    return false;
}

//------------------ owner_of_node: node index of the innermost enclosing FunctionDef (0xFFFFFFFF = file scope)
inline uint32_t owner_of_node(const AnalysisState& state, uint32_t node_idx)
{
    auto it = state.node_func_owner.find(node_idx);
    return it != state.node_func_owner.end() ? it->second : 0xFFFFFFFFu;
}

}

#endif
