// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  optimizer.h · Optimizer Header             │
// └─────────────────────────────────────────────┘

#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "../syntax/nodes.h"
#include "../syntax/parser.h"
#include "../codegen/codegen.h"
#include "analysis_state.h"
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <cstring>
#include <string_view>

namespace clx {


//------------------ Optimizer: runs analysis passes over an AST, populating AnalysisState for CodeEmitter
class Optimizer {
public:
    //------------------ Optimizer: constructs optimizer for a given AST context and analysis state
    Optimizer(const ASTContext& context, AnalysisState& analysis);

    //------------------ run: executes all analysis passes on the AST
    void run(const ASTContext& ctx, uint32_t root_node);

private:
    const ASTContext* ctx;
    AnalysisState& state;
};

//------------------ yields_number: returns true if a node always evaluates to a number
inline bool yields_number(const ASTContext& ctx, const AnalysisState& state, uint32_t node_idx,
                           const std::set<std::string_view>* known_numbers = nullptr,
                           std::string_view self_name = "",
                           const std::set<std::string_view>* param_numbers = nullptr,
                           int* depth = nullptr) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size()) return false;
    int dummy = 0;
    if (!depth) depth = &dummy;
    if (++*depth > 100) { --*depth; return false; }

    const auto& n = ctx.nodes[node_idx];
    bool result = false;

    if (n.type == NodeType::Number || n.type == NodeType::Integer)
        { result = true; goto done; }

    if (n.type == NodeType::IntrinsicCall)
        { result = strcmp(n.as.intrinsic_call.cname, "__clx_type") != 0 && strcmp(n.as.intrinsic_call.cname, "__clx_tostring") != 0; goto done; }

    if (n.type == NodeType::ParenExpression)
        { result = yields_number(ctx, state, n.as.paren_expr.expr, known_numbers, self_name, param_numbers, depth); goto done; }

    if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Len))
        { result = true; goto done; }
    if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Minus))
        { result = yields_number(ctx, state, n.as.unary_op.expr, known_numbers, self_name, param_numbers, depth); goto done; }

    if (n.type == NodeType::BinaryOp) {
        int op = n.as.bin_op.op;

        if ((op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) || (op >= static_cast<int>(BinaryOp::Mod) && op <= static_cast<int>(BinaryOp::Shr))) {
            result = yields_number(ctx, state, n.as.bin_op.left,  known_numbers, self_name, param_numbers, depth) &&
                     yields_number(ctx, state, n.as.bin_op.right, known_numbers, self_name, param_numbers, depth);
            goto done;
        }


        if (op == static_cast<int>(BinaryOp::Or) && yields_number(ctx, state, n.as.bin_op.right, known_numbers, self_name, param_numbers, depth)) {
            uint32_t left = n.as.bin_op.left;
            if (ctx.nodes[left].type == NodeType::CallExpression ||
                yields_number(ctx, state, left, known_numbers, self_name, param_numbers, depth))
                { result = true; goto done; }
        }
        result = false; goto done;
    }

    if (n.type == NodeType::TableAccess) {
        uint32_t tbl_idx = n.as.table_access.table;


        if (tbl_idx < ctx.nodes.size() && ctx.nodes[tbl_idx].type == NodeType::Identifier) {
            std::string_view tn(ctx.nodes[tbl_idx].as.ident.name, ctx.nodes[tbl_idx].as.ident.length);
            if (state.pure_numeric_arrays.count(tn)) {

                result = yields_number(ctx, state, n.as.table_access.key, known_numbers, self_name, param_numbers, depth);
                goto done;
            }


            auto it = state.numeric_table_fields.find(tn);
            if (it == state.numeric_table_fields.end()) {

                for (const auto& nd : ctx.nodes) {
                    if (nd.type != NodeType::LocalDecl) continue;
                    for (uint32_t ii = 0; ii < nd.as.local_decl.ident_count; ++ii) {
                        uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + ii];
                        if (ctx.nodes[idi].type != NodeType::Identifier) continue;
                        if (std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length) != tn) continue;
                        uint32_t vi = (ii < nd.as.local_decl.value_count)
                            ? ctx.block_statements[nd.as.local_decl.first_value + ii] : 0xFFFFFFFF;
                        if (vi != 0xFFFFFFFF && ctx.nodes[vi].type == NodeType::TableAccess) {
                            uint32_t src_tbl = ctx.nodes[vi].as.table_access.table;
                            if (ctx.nodes[src_tbl].type == NodeType::Identifier) {
                                std::string_view src_nm(ctx.nodes[src_tbl].as.ident.name, ctx.nodes[src_tbl].as.ident.length);
                                auto sit = state.numeric_table_fields.find(src_nm);
                                if (sit != state.numeric_table_fields.end()) {
                                    if (ctx.nodes[n.as.table_access.key].type == NodeType::String) {
                                        std::string_view fn(ctx.nodes[n.as.table_access.key].as.string.text,
                                                            ctx.nodes[n.as.table_access.key].as.string.length);
                                        if (sit->second.count(fn)) { result = true; goto done; }
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            } else {
                if (ctx.nodes[n.as.table_access.key].type == NodeType::String) {
                    std::string_view fn(ctx.nodes[n.as.table_access.key].as.string.text,
                                        ctx.nodes[n.as.table_access.key].as.string.length);
                    if (it->second.count(fn)) { result = true; goto done; }
                }
            }
        }


        if (tbl_idx < ctx.nodes.size() && ctx.nodes[tbl_idx].type == NodeType::TableAccess) {
            const auto& inner_acc = ctx.nodes[tbl_idx].as.table_access;
            if (ctx.nodes[inner_acc.table].type == NodeType::Identifier) {
                std::string_view tn(ctx.nodes[inner_acc.table].as.ident.name, ctx.nodes[inner_acc.table].as.ident.length);
                auto it = state.numeric_table_fields.find(tn);
                if (it != state.numeric_table_fields.end()) {
                    if (ctx.nodes[n.as.table_access.key].type == NodeType::String) {
                        std::string_view fn(ctx.nodes[n.as.table_access.key].as.string.text,
                                            ctx.nodes[n.as.table_access.key].as.string.length);
                        if (it->second.count(fn)) { result = true; goto done; }
                    }
                }
            }
        }

        result = false; goto done;
    }

    if (n.type == NodeType::CallExpression) {
        uint32_t tgt = n.as.call_expr.target;
        if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
            std::string_view fname(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (state.native_return_funcs.count(fname)) { result = true; goto done; }
            if (!self_name.empty() && fname == self_name)         { result = true; goto done; }
        }
        result = false; goto done;
    }

    if (n.type == NodeType::Identifier) {
        std::string_view name(n.as.ident.name, n.as.ident.length);
        if (n.as.ident.is_global && state.global_constants.count(name))
            { result = true; goto done; }
        if (std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                != state.native_numbers.end())
            { result = true; goto done; }
        if (known_numbers  && known_numbers->count(name))  { result = true; goto done; }
        if (param_numbers  && param_numbers->count(name))  { result = true; goto done; }
    }

    result = false;
done:
    --*depth;
    return result;
}


}

#endif
