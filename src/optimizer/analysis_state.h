// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  analysis_state.h · Shared Analysis State   │
// └─────────────────────────────────────────────┘
//
// AnalysisState holds every piece of information the Optimizer discovers
// about a translation unit and that CodeEmitter later consumes while
// generating C++.
// A single AnalysisState is constructed per compiled file and passed by reference
// to both Optimizer (which populates it) and CodeEmitter (which reads it).
//
// AnalysisState is constructed per compiled file and passed by reference to
// both Optimizer (which populates it) and CodeEmitter (which reads it).
//
// Keeping this as one cohesive struct (rather than scattering the fields
// back onto Optimizer/CodeEmitter individually) mirrors how the two classes
// actually use the data: Optimizer's passes cross-reference each other's
// findings, and CodeEmitter's emission logic reads whichever fields are
// relevant to the node it's emitting.

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
    std::set<std::string_view> native_return_funcs;
    std::map<uint32_t, std::set<std::string>> param_numbers;
    std::vector<std::string> param_names;
    std::map<uint32_t, uint32_t> table_presize;
    std::map<std::string_view, double> global_constants;
    std::set<uint32_t> bce_safe_nodes;
    std::set<std::string_view> pure_numeric_arrays;
    std::set<std::string_view>
        tables_with_dynamic_length; // tables whose # may change (setmetatable, table.insert, table.remove)
    std::map<std::string_view, size_t> known_table_lengths; // table name → known length (# operator)
    std::map<std::string_view, std::set<uint32_t>>
        pure_numeric_func_params; // param name → set of FunctionDef node indices
    std::map<uint32_t, uint32_t> node_func_owner; // node index → FunctionDef node index that owns it
    std::set<std::string, std::less<>> native_integers;
    std::set<std::string_view> int_returning_funcs;
    std::set<std::string, std::less<>> int_typed_locals;

    //------------------ Arena analysis data
    std::set<std::string_view> escaping_vars;
    std::set<uint32_t> arena_safe_table_nodes;
    std::map<uint32_t, uint32_t> arena_table_sizes; // func_node → total arena bytes

    //------------------ Per-function analysis data
    std::map<std::string_view, std::set<std::string_view>> numeric_table_fields;
    std::set<std::string_view> direct_callables;
    std::set<std::string_view> fast_callables;
    std::map<std::string_view, uint32_t> func_param_counts;
    std::map<std::string_view, std::vector<bool>> func_param_native;
    std::set<std::string_view> reassigned_vars;
    std::set<std::string_view> constant_upvalues;
    std::set<std::string_view> string_builders;
    std::set<std::string_view> global_string_builders;
    std::set<std::string_view> module_string_builders;
    std::map<uint32_t, uint32_t> goto_targets;
    std::set<std::string_view> hoisted_locals;
    std::unordered_map<uint32_t, std::string> hoisted_lookups;
    std::unordered_map<std::string, std::string> hoisted_cfuncs;
    std::unordered_map<std::string, std::string> builtin_aliases;

    //------------------ Codegen emission state (mutated as CodeEmitter walks the tree)
    bool skip_block_braces = false;
    bool emit_raw_lambda = false;
    bool emit_fast_lambda = false;
    bool in_function_def = false;
    bool in_fast_function = false;
    bool expect_multivalue = false;
    std::string_view current_fast_func;
    std::string ref_capture;
    uint32_t current_func_body = 0xFFFFFFFF;
    uint32_t current_arena_func = 0xFFFFFFFF; // node index of current function if it has an arena
};

//------------------ is_purely_integer_expr: returns true if a node always evaluates to an integer.
inline bool is_purely_integer_expr(const ASTContext &ctx, const AnalysisState &state, uint32_t node_idx) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto &n = ctx.nodes[node_idx];
    if (n.type == NodeType::Integer)
        return true;
    if (n.type == NodeType::Number) {
        double v = n.as.number.val;
        return v == static_cast<double>(static_cast<int64_t>(v)) && v >= -9.0e15 && v <= 9.0e15;
    }
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

}

#endif
