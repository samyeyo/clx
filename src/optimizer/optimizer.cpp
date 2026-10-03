// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  optimizer.cpp · AST Optimizer              │
// └─────────────────────────────────────────────┘

#include "optimizer.h"
#include "passes.h"
#include "../../include/clx_runtime.h"
#include "../codegen/codegen.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace clx {

//------------------ Optimizer constructor
Optimizer::Optimizer(const ASTContext &context, AnalysisState &analysis)
    : ctx(&context)
    , state(analysis) { }

//------------------ get_ast_string — convert AST node to string for analysis
std::string get_ast_string(const ASTContext &ctx, uint32_t node_idx) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return "";
    const auto &n = ctx.nodes[node_idx];
    if (n.type == NodeType::Identifier)
        return std::string(n.as.ident.name, n.as.ident.length);
    if (n.type == NodeType::TableAccess) {
        std::string t = get_ast_string(ctx, n.as.table_access.table);
        std::string k = get_ast_string(ctx, n.as.table_access.key);
        if (t.empty() || k.empty())
            return "";
        return t + "[" + k + "]";
    }
    if (n.type == NodeType::BinaryOp) {
        std::string l = get_ast_string(ctx, n.as.bin_op.left);
        std::string r = get_ast_string(ctx, n.as.bin_op.right);
        if (l.empty() || r.empty())
            return "";
        if (n.as.bin_op.op == static_cast<int>(BinaryOp::Add))
            return l + " + " + r;
        if (n.as.bin_op.op == static_cast<int>(BinaryOp::Sub))
            return l + " - " + r;
    }
    return "";
}

//------------------ function_returns_native — check if function always returns native number
bool function_returns_native(const ASTContext &ctx, const AnalysisState &state, uint32_t func_idx,
    std::string_view self_name, const std::set<std::string_view> *known_numbers) {
    if (func_idx >= ctx.nodes.size())
        return false;
    const auto &fn = ctx.nodes[func_idx];
    if (fn.type != NodeType::FunctionDef)
        return false;

    std::set<std::string_view> param_numbers;
    if (!self_name.empty() && state.func_param_native.count(self_name)) {
        const auto &pn = state.func_param_native.at(self_name);
        for (size_t p = 0; p < fn.as.func_def.param_count; ++p) {
            if (p < pn.size() && pn[p]) {
                uint32_t p_idx = ctx.block_statements[fn.as.func_def.first_param + p];
                std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                param_numbers.insert(pname);
            }
        }
    }

    bool has_return = false;
    bool all_returns_native = true;

    auto check_block = [&](auto &self, uint32_t block_idx) -> void {
        if (block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size())
            return;
        const auto &block = ctx.nodes[block_idx];
        if (block.type != NodeType::Block)
            return;

        for (uint32_t i = 0; i < block.as.block.count; ++i) {
            uint32_t si = ctx.block_statements[block.as.block.first_statement + i];
            if (si >= ctx.nodes.size())
                continue;

            const auto &stmt = ctx.nodes[si];
            if (stmt.type == NodeType::ReturnStatement) {
                has_return = true;
                if (stmt.as.return_stmt.value_count != 1) {
                    all_returns_native = false;
                } else {
                    uint32_t vi = ctx.block_statements[stmt.as.return_stmt.first_value];
                    if (!yields_number(ctx, state, vi, known_numbers, self_name, &param_numbers)) {
                        all_returns_native = false;
                    }
                }
            } else if (stmt.type == NodeType::IfStatement) {
                self(self, stmt.as.if_stmt.then_block);
                if (stmt.as.if_stmt.else_block != 0xFFFFFFFF) {
                    if (ctx.nodes[stmt.as.if_stmt.else_block].type == NodeType::IfStatement) {
                        uint32_t dummy_block = stmt.as.if_stmt.else_block;
                        while (dummy_block != 0xFFFFFFFF && ctx.nodes[dummy_block].type == NodeType::IfStatement) {
                            self(self, ctx.nodes[dummy_block].as.if_stmt.then_block);
                            dummy_block = ctx.nodes[dummy_block].as.if_stmt.else_block;
                        }
                        if (dummy_block != 0xFFFFFFFF)
                            self(self, dummy_block);
                    } else {
                        self(self, stmt.as.if_stmt.else_block);
                    }
                }
            } else if (stmt.type == NodeType::WhileStatement) {
                self(self, stmt.as.while_stmt.body_block);
            } else if (stmt.type == NodeType::RepeatStatement) {
                self(self, stmt.as.repeat_stmt.body_block);
            } else if (stmt.type == NodeType::ForStatement) {
                self(self, stmt.as.for_stmt.body_block);
            } else if (stmt.type == NodeType::GenericForStatement) {
                self(self, stmt.as.generic_for.body_block);
            } else if (stmt.type == NodeType::DoStatement) {
                self(self, stmt.as.do_stmt.body_block);
            }
        }
    };

    check_block(check_block, fn.as.func_def.body_block);
    return has_return && all_returns_native;
}

//------------------ is_literal_number — check if node is a literal integer or number
bool is_literal_number(const ASTContext &ctx, uint32_t node_idx, double &out_d, int64_t &out_i, bool &out_is_int) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto &n = ctx.nodes[node_idx];
    if (n.type == NodeType::Integer) {
        out_i = n.as.integer.val;
        out_d = static_cast<double>(out_i);
        out_is_int = true;
        return true;
    }
    if (n.type == NodeType::Number) {
        out_d = n.as.number.val;
        out_i = static_cast<int64_t>(out_d);
        out_is_int = false;
        return true;
    }
    return false;
}

//------------------ Optimizer::run — main optimization entry point
void Optimizer::run(const ASTContext &ctx, uint32_t root_node) {
    state.native_numbers.clear();
    state.string_pool.clear();
    state.string_pool_index.clear();
    state.table_presize.clear();
    state.global_constants.clear();
    state.bce_safe_nodes.clear();
    state.direct_callables.clear();
    state.fast_callables.clear();
    state.native_direct_funcs.clear();
    state.table_typed_locals.clear();
    state.native_return_funcs.clear();
    state.func_param_counts.clear();
    state.func_param_native.clear();
    state.dead_stmts.clear();

    PassScratch scratch;
    pass_node_func_owner(ctx, state, root_node);
    pass_goto_labels(ctx, state, root_node);
    pass_name_scans(ctx, state, scratch, root_node);
    pass_constants_and_bounds(ctx, state, scratch, root_node);
    pass_callables(ctx, state, scratch, root_node);
    pass_pure_arrays(ctx, state, scratch, root_node);
    pass_array_safety(ctx, state, scratch, root_node);
    pass_table_fields(ctx, state, scratch, root_node);
    pass_numeric_params(ctx, state, scratch, root_node);
    pass_arena(ctx, state, root_node);
    pass_int_returns_masks(ctx, state, scratch, root_node);
    pass_local_hygiene(ctx, state, root_node);
}

}
