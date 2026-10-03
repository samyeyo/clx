// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  owners.cpp · Ownership & scope passes      │
// └─────────────────────────────────────────────┘

#include "optimizer.h"
#include "passes.h"
#include "../../include/clx_runtime.h"
#include "../codegen/codegen.h"
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clx {

//------------------ pass_node_func_owner: map every node to its innermost enclosing FunctionDef
void pass_node_func_owner(const ASTContext &ctx, AnalysisState &state, uint32_t root_node) {
    //------------------ Map every node to its innermost enclosing FunctionDef. Purely structural
    state.node_func_owner.clear();
    {
        std::vector<std::pair<uint32_t, uint32_t>> stk;
        stk.emplace_back(root_node, 0xFFFFFFFFu);
        while (!stk.empty()) {
            auto [ni, owner] = stk.back();
            stk.pop_back();
            if (ni == 0xFFFFFFFF || ni >= ctx.nodes.size())
                continue;
            if (ctx.nodes[ni].type == NodeType::FunctionDef)
                owner = ni;
            if (owner != 0xFFFFFFFFu)
                state.node_func_owner[ni] = owner;
            const auto &nn = ctx.nodes[ni];
            auto push = [&](uint32_t idx) { stk.emplace_back(idx, owner); };
            switch (nn.type) {
            case NodeType::Block:
                for (uint32_t bi = 0; bi < nn.as.block.count; ++bi)
                    push(ctx.block_statements[nn.as.block.first_statement + bi]);
                break;
            case NodeType::LocalDecl:
                for (uint32_t vi = 0; vi < nn.as.local_decl.value_count; ++vi)
                    push(ctx.block_statements[nn.as.local_decl.first_value + vi]);
                break;
            case NodeType::GlobalDeclStatement:
                for (uint32_t vi = 0; vi < nn.as.global_decl.value_count; ++vi)
                    push(ctx.block_statements[nn.as.global_decl.first_value + vi]);
                break;
            case NodeType::Assignment:
                for (uint32_t ti = 0; ti < nn.as.assign.target_count; ++ti)
                    push(ctx.block_statements[nn.as.assign.first_target + ti]);
                for (uint32_t vi = 0; vi < nn.as.assign.value_count; ++vi)
                    push(ctx.block_statements[nn.as.assign.first_value + vi]);
                break;
            case NodeType::BinaryOp:
                push(nn.as.bin_op.left);
                push(nn.as.bin_op.right);
                break;
            case NodeType::UnaryOp:
                push(nn.as.unary_op.expr);
                break;
            case NodeType::ParenExpression:
                push(nn.as.paren_expr.expr);
                break;
            case NodeType::CallExpression:
                push(nn.as.call_expr.target);
                for (uint32_t ai = 0; ai < nn.as.call_expr.arg_count; ++ai)
                    push(ctx.block_statements[nn.as.call_expr.first_arg + ai]);
                break;
            case NodeType::IntrinsicCall:
                for (uint32_t ai = 0; ai < nn.as.intrinsic_call.arg_count; ++ai)
                    push(ctx.block_statements[nn.as.intrinsic_call.first_arg + ai]);
                break;
            case NodeType::TableConstructor:
                for (uint32_t ei = 0; ei < nn.as.table_cons.count; ++ei) {
                    push(ctx.block_statements[nn.as.table_cons.first_item + ei * 2]);
                    push(ctx.block_statements[nn.as.table_cons.first_item + ei * 2 + 1]);
                }
                break;
            case NodeType::TableAccess:
                push(nn.as.table_access.table);
                push(nn.as.table_access.key);
                break;
            case NodeType::IfStatement:
                push(nn.as.if_stmt.condition);
                if (nn.as.if_stmt.then_block != 0xFFFFFFFF)
                    push(nn.as.if_stmt.then_block);
                if (nn.as.if_stmt.else_block != 0xFFFFFFFF)
                    push(nn.as.if_stmt.else_block);
                break;
            case NodeType::WhileStatement:
                push(nn.as.while_stmt.condition);
                push(nn.as.while_stmt.body_block);
                break;
            case NodeType::RepeatStatement:
                push(nn.as.repeat_stmt.condition);
                push(nn.as.repeat_stmt.body_block);
                break;
            case NodeType::ForStatement:
                push(nn.as.for_stmt.var_ident);
                push(nn.as.for_stmt.start_expr);
                push(nn.as.for_stmt.limit_expr);
                if (nn.as.for_stmt.step_expr != 0xFFFFFFFF)
                    push(nn.as.for_stmt.step_expr);
                push(nn.as.for_stmt.body_block);
                break;
            case NodeType::GenericForStatement:
                for (uint32_t vi = 0; vi < nn.as.generic_for.var_count; ++vi)
                    push(ctx.block_statements[nn.as.generic_for.first_var + vi]);
                for (uint32_t ii = 0; ii < nn.as.generic_for.iter_count; ++ii)
                    push(ctx.block_statements[nn.as.generic_for.first_iter + ii]);
                push(nn.as.generic_for.body_block);
                break;
            case NodeType::DoStatement:
                push(nn.as.do_stmt.body_block);
                break;
            case NodeType::FunctionDef:
                push(nn.as.func_def.body_block);
                break;
            case NodeType::ReturnStatement:
                for (uint32_t vi = 0; vi < nn.as.return_stmt.value_count; ++vi)
                    push(ctx.block_statements[nn.as.return_stmt.first_value + vi]);
                break;
            default:
                break;
            }
        }
    }
}

//------------------ pass_goto_labels: resolve goto/label targets into state.goto_targets
void pass_goto_labels(const ASTContext &ctx, AnalysisState &state, uint32_t root_node) {
    state.goto_targets.clear();
    auto resolve_labels = [&](auto &self, uint32_t n_idx, std::map<std::string_view, uint32_t> visible) -> void {
        if (n_idx == 0xFFFFFFFF || n_idx >= ctx.nodes.size())
            return;
        const auto &n = ctx.nodes[n_idx];

        if (n.type == NodeType::Block) {
            for (uint32_t i = 0; i < n.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[n.as.block.first_statement + i];
                if (stmt_idx >= ctx.nodes.size())
                    continue;
                if (ctx.nodes[stmt_idx].type == NodeType::LabelStatement) {
                    uint32_t name_idx = ctx.nodes[stmt_idx].as.label_stmt.name_ident;
                    std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);
                    visible[lname] = stmt_idx;
                }
            }
            for (uint32_t i = 0; i < n.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[n.as.block.first_statement + i];
                if (stmt_idx >= ctx.nodes.size())
                    continue;
                const auto &stmt = ctx.nodes[stmt_idx];
                if (stmt.type == NodeType::GotoStatement) {
                    uint32_t name_idx = stmt.as.goto_stmt.name_ident;
                    std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);
                    if (visible.count(lname)) {
                        state.goto_targets[stmt_idx] = visible[lname];
                    } else {
                        throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(stmt.line)
                            + ": no visible label '" + std::string(lname) + "' for <goto>");
                    }
                } else {
                    self(self, stmt_idx, visible);
                }
            }
        } else if (n.type == NodeType::FunctionDef) {
            std::map<std::string_view, uint32_t> empty;
            self(self, n.as.func_def.body_block, empty);
        } else if (n.type == NodeType::IfStatement) {
            self(self, n.as.if_stmt.then_block, visible);
            self(self, n.as.if_stmt.else_block, visible);
        } else if (n.type == NodeType::WhileStatement) {
            self(self, n.as.while_stmt.body_block, visible);
        } else if (n.type == NodeType::RepeatStatement) {
            self(self, n.as.repeat_stmt.body_block, visible);
        } else if (n.type == NodeType::ForStatement) {
            self(self, n.as.for_stmt.body_block, visible);
        } else if (n.type == NodeType::GenericForStatement) {
            self(self, n.as.generic_for.body_block, visible);
        } else if (n.type == NodeType::DoStatement) {
            self(self, n.as.do_stmt.body_block, visible);
        } else if (n.type == NodeType::LocalDecl || n.type == NodeType::Assignment
            || n.type == NodeType::GlobalDeclStatement) {
            uint32_t v_count = (n.type == NodeType::LocalDecl)
                ? n.as.local_decl.value_count
                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count : n.as.assign.value_count);
            uint32_t first_v = (n.type == NodeType::LocalDecl)
                ? n.as.local_decl.first_value
                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value : n.as.assign.first_value);
            for (uint32_t i = 0; i < v_count; ++i)
                self(self, ctx.block_statements[first_v + i], visible);
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
                self(self, ctx.block_statements[n.as.return_stmt.first_value + i], visible);
        } else if (n.type == NodeType::CallExpression) {
            self(self, n.as.call_expr.target, visible);
            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i)
                self(self, ctx.block_statements[n.as.call_expr.first_arg + i], visible);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2], visible);
                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1], visible);
            }
        } else if (n.type == NodeType::BinaryOp) {
            self(self, n.as.bin_op.left, visible);
            self(self, n.as.bin_op.right, visible);
        } else if (n.type == NodeType::UnaryOp) {
            self(self, n.as.unary_op.expr, visible);
        } else if (n.type == NodeType::ParenExpression) {
            self(self, n.as.paren_expr.expr, visible);
        }
    };
    std::map<std::string_view, uint32_t> root_lbls;
    resolve_labels(resolve_labels, root_node, root_lbls);
}

//------------------ pass_name_scans: reassigned/assigned targets and numeric-for counter names
void pass_name_scans(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &for_vars = sk.for_vars;
    state.reassigned_vars.clear();
    std::map<std::string_view, uint32_t> var_assign_counts;
    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement
            || node.type == NodeType::Assignment) {
            uint32_t t_count = (node.type == NodeType::LocalDecl)
                ? node.as.local_decl.ident_count
                : (node.type == NodeType::GlobalDeclStatement ? node.as.global_decl.ident_count
                                                              : node.as.assign.target_count);
            uint32_t first_t = (node.type == NodeType::LocalDecl)
                ? node.as.local_decl.first_ident
                : (node.type == NodeType::GlobalDeclStatement ? node.as.global_decl.first_ident
                                                              : node.as.assign.first_target);
            for (uint32_t i = 0; i < t_count; ++i) {
                uint32_t t_idx = ctx.block_statements[first_t + i];
                if (ctx.nodes[t_idx].type == NodeType::Identifier) {
                    std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    var_assign_counts[name]++;
                }
            }
        }
    }
    state.assigned_targets.clear();
    for (const auto &pair : var_assign_counts) {
        state.assigned_targets.insert(pair.first);
        if (pair.second > 1)
            state.reassigned_vars.insert(pair.first);
    }

    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::ForStatement && node.as.for_stmt.var_ident < ctx.nodes.size()) {
            const auto &vn = ctx.nodes[node.as.for_stmt.var_ident];
            if (vn.type == NodeType::Identifier)
                for_vars.insert(std::string_view(vn.as.ident.name, vn.as.ident.length));
        }
    }
    state.for_counter_names = for_vars;
}

//------------------ pass_local_hygiene: goto-crossed locals, definite-table locals, dead literal tables
void pass_local_hygiene(const ASTContext &ctx, AnalysisState &state, uint32_t root_node) {
    //------------------ Goto-crossed locals: a goto can skip the initializer, so later uses cannot trust it; exclude from table_typed_locals.
    std::set<std::string_view> goto_crossed;
    {
        auto walk_block = [&](auto &self, uint32_t bi) -> void {
            if (bi == 0xFFFFFFFF || bi >= ctx.nodes.size() || ctx.nodes[bi].type != NodeType::Block)
                return;
            const auto &blk = ctx.nodes[bi];
            bool after_goto = false;
            for (uint32_t i = 0; i < blk.as.block.count; ++i) {
                uint32_t si = ctx.block_statements[blk.as.block.first_statement + i];
                if (si >= ctx.nodes.size())
                    continue;
                const auto &sn = ctx.nodes[si];
                if (sn.type == NodeType::GotoStatement) {
                    after_goto = true;
                    continue;
                }
                if (sn.type == NodeType::LabelStatement) {
                    after_goto = false;
                    continue;
                }
                if (after_goto && sn.type == NodeType::LocalDecl) {
                    for (uint32_t j = 0; j < sn.as.local_decl.ident_count; ++j) {
                        uint32_t idi = ctx.block_statements[sn.as.local_decl.first_ident + j];
                        if (ctx.nodes[idi].type == NodeType::Identifier)
                            goto_crossed.insert(
                                std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length));
                    }
                }
                if (sn.type == NodeType::Block)
                    self(self, si);
                else if (sn.type == NodeType::IfStatement) {
                    self(self, sn.as.if_stmt.then_block);
                    if (sn.as.if_stmt.else_block != 0xFFFFFFFF)
                        self(self, sn.as.if_stmt.else_block);
                } else if (sn.type == NodeType::WhileStatement)
                    self(self, sn.as.while_stmt.body_block);
                else if (sn.type == NodeType::RepeatStatement)
                    self(self, sn.as.repeat_stmt.body_block);
                else if (sn.type == NodeType::ForStatement)
                    self(self, sn.as.for_stmt.body_block);
                else if (sn.type == NodeType::GenericForStatement)
                    self(self, sn.as.generic_for.body_block);
                else if (sn.type == NodeType::DoStatement)
                    self(self, sn.as.do_stmt.body_block);
            }
        };
        walk_block(walk_block, root_node);
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            if (ctx.nodes[i].type == NodeType::FunctionDef)
                walk_block(walk_block, ctx.nodes[i].as.func_def.body_block);
        }
    }

    //------------------ Definite-table locals: a single unique-name `local x = {...}` never reassigned; uses may skip the table type check.
    {
        std::map<std::string_view, int> decl_counts;
        auto count_ident = [&](uint32_t idx) {
            if (idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::Identifier)
                decl_counts[std::string_view(ctx.nodes[idx].as.ident.name, ctx.nodes[idx].as.ident.length)]++;
        };
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            if (nd.type == NodeType::LocalDecl) {
                for (uint32_t j = 0; j < nd.as.local_decl.ident_count; ++j)
                    count_ident(ctx.block_statements[nd.as.local_decl.first_ident + j]);
            } else if (nd.type == NodeType::GlobalDeclStatement) {
                for (uint32_t j = 0; j < nd.as.global_decl.ident_count; ++j)
                    count_ident(ctx.block_statements[nd.as.global_decl.first_ident + j]);
            } else if (nd.type == NodeType::FunctionDef) {
                for (uint32_t j = 0; j < nd.as.func_def.param_count; ++j)
                    count_ident(ctx.block_statements[nd.as.func_def.first_param + j]);
                if (nd.as.func_def.is_vararg && nd.as.func_def.named_vararg_ident != 0xFFFFFFFF)
                    count_ident(nd.as.func_def.named_vararg_ident);
            } else if (nd.type == NodeType::ForStatement) {
                count_ident(nd.as.for_stmt.var_ident);
            } else if (nd.type == NodeType::GenericForStatement) {
                for (uint32_t j = 0; j < nd.as.generic_for.var_count; ++j)
                    count_ident(ctx.block_statements[nd.as.generic_for.first_var + j]);
            }
        }
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            if (nd.type != NodeType::LocalDecl)
                continue;
            if (nd.as.local_decl.ident_count != 1 || nd.as.local_decl.value_count != 1)
                continue;
            uint32_t fi = ctx.block_statements[nd.as.local_decl.first_ident];
            uint32_t fv = ctx.block_statements[nd.as.local_decl.first_value];
            if (fi >= ctx.nodes.size() || fv >= ctx.nodes.size())
                continue;
            if (ctx.nodes[fi].type != NodeType::Identifier || ctx.nodes[fi].as.ident.is_global)
                continue;
            if (ctx.nodes[fi].as.ident.is_captured)
                continue;
            if (ctx.nodes[fv].type != NodeType::TableConstructor)
                continue;
            std::string_view nm(ctx.nodes[fi].as.ident.name, ctx.nodes[fi].as.ident.length);
            if (state.reassigned_vars.count(nm))
                continue;
            auto dc = decl_counts.find(nm);
            if (dc == decl_counts.end() || dc->second != 1)
                continue;
            if (goto_crossed.count(nm))
                continue;
            state.table_typed_locals.insert(std::string(nm));
        }
    }

    //------------------ Dead literal-table locals: single-occurrence literal-only table decls are unobservable; codegen drops them.
    {
        std::map<std::string_view, uint32_t> ident_uses;
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            if (nd.type == NodeType::Identifier)
                ident_uses[std::string_view(nd.as.ident.name, nd.as.ident.length)]++;
        }
        auto literal_ctor = [&](uint32_t vidx) -> bool {
            const auto &tc = ctx.nodes[vidx];
            for (uint32_t i = 0; i < tc.as.table_cons.count; ++i) {
                uint32_t k = ctx.block_statements[tc.as.table_cons.first_item + i * 2];
                uint32_t v = ctx.block_statements[tc.as.table_cons.first_item + i * 2 + 1];
                if (k != 0xFFFFFFFF) {
                    NodeType kt = ctx.nodes[k].type;
                    if (kt != NodeType::String && kt != NodeType::Integer && kt != NodeType::Number)
                        return false;
                }
                switch (ctx.nodes[v].type) {
                case NodeType::Integer:
                case NodeType::Number:
                case NodeType::String:
                case NodeType::TrueLiteral:
                case NodeType::FalseLiteral:
                case NodeType::NilLiteral:
                    break;
                default:
                    return false;
                }
            }
            return true;
        };
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            if (nd.type != NodeType::LocalDecl)
                continue;
            if (nd.as.local_decl.ident_count != 1 || nd.as.local_decl.value_count != 1)
                continue;
            uint32_t fi = ctx.block_statements[nd.as.local_decl.first_ident];
            uint32_t fv = ctx.block_statements[nd.as.local_decl.first_value];
            if (fi >= ctx.nodes.size() || fv >= ctx.nodes.size())
                continue;
            if (ctx.nodes[fi].type != NodeType::Identifier || ctx.nodes[fi].as.ident.is_global)
                continue;
            if (ctx.nodes[fi].as.ident.is_captured)
                continue;
            if (ctx.nodes[fv].type != NodeType::TableConstructor || !literal_ctor(fv))
                continue;
            std::string_view nm(ctx.nodes[fi].as.ident.name, ctx.nodes[fi].as.ident.length);
            if (state.reassigned_vars.count(nm) || goto_crossed.count(nm))
                continue;
            auto uc = ident_uses.find(nm);
            if (uc == ident_uses.end() || uc->second != 1)
                continue;
            state.dead_stmts.insert(i);
        }
    }
}

}
