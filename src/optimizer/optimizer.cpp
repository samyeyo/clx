// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  optimizer.cpp · AST Optimizer              │
// └─────────────────────────────────────────────┘

#include "optimizer.h"
#include "../codegen/codegen.h"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

namespace clx {

//------------------ Optimizer constructor
Optimizer::Optimizer(const ASTContext& context, AnalysisState& analysis) : ctx(&context), state(analysis) {}

//------------------ get_ast_string — convert AST node to string for analysis
static std::string get_ast_string(const ASTContext& ctx, uint32_t node_idx) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size()) return "";
    const auto& n = ctx.nodes[node_idx];
    if (n.type == NodeType::Identifier) return std::string(n.as.ident.name, n.as.ident.length);
    if (n.type == NodeType::TableAccess) {
        std::string t = get_ast_string(ctx, n.as.table_access.table);
        std::string k = get_ast_string(ctx, n.as.table_access.key);
        if (t.empty() || k.empty()) return "";
        return t + "[" + k + "]";
    }
    if (n.type == NodeType::BinaryOp) {
        std::string l = get_ast_string(ctx, n.as.bin_op.left);
        std::string r = get_ast_string(ctx, n.as.bin_op.right);
        if (l.empty() || r.empty()) return "";
        if (n.as.bin_op.op == static_cast<int>(BinaryOp::Add)) return l + " + " + r;
        if (n.as.bin_op.op == static_cast<int>(BinaryOp::Sub)) return l + " - " + r;
    }
    return "";
}

//------------------ function_returns_native — check if function always returns native number
static bool function_returns_native(const ASTContext& ctx, const AnalysisState& state, uint32_t func_idx,
                             std::string_view self_name,
                             const std::set<std::string_view>* known_numbers) {
    if (func_idx >= ctx.nodes.size()) return false;
    const auto& fn = ctx.nodes[func_idx];
    if (fn.type != NodeType::FunctionDef) return false;

    std::set<std::string_view> param_numbers;
    if (!self_name.empty() && state.func_param_native.count(self_name)) {
        const auto& pn = state.func_param_native.at(self_name);
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

    auto check_block = [&](auto& self, uint32_t block_idx) -> void {
        if (block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size()) return;
        const auto& block = ctx.nodes[block_idx];
        if (block.type != NodeType::Block) return;

        for (uint32_t i = 0; i < block.as.block.count; ++i) {
            uint32_t si = ctx.block_statements[block.as.block.first_statement + i];
            if (si >= ctx.nodes.size()) continue;

            const auto& stmt = ctx.nodes[si];
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
                        if (dummy_block != 0xFFFFFFFF) self(self, dummy_block);
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
static bool is_literal_number(const ASTContext& ctx, uint32_t node_idx, double& out_d, int64_t& out_i, bool& out_is_int) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size()) return false;
    const auto& n = ctx.nodes[node_idx];
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
void Optimizer::run(const ASTContext& ctx, uint32_t root_node) {

    state.native_numbers.clear();
    state.string_pool.clear();
    state.table_presize.clear();
    state.global_constants.clear();
    state.bce_safe_nodes.clear();
    state.direct_callables.clear();
    state.fast_callables.clear();
    state.native_return_funcs.clear();
    state.func_param_counts.clear();
    state.func_param_native.clear();

    std::set<std::string_view> known_numbers;
    std::set<std::string_view> disqualified;

    std::map<std::string, uint32_t> array_bounds;
    std::map<std::string, uint32_t> loop_limits;
    std::map<std::string, bool> loop_limit_conflicts;

    state.goto_targets.clear();
    auto resolve_labels = [&](auto& self, uint32_t n_idx, std::map<std::string_view, uint32_t> visible) -> void {
        if (n_idx == 0xFFFFFFFF || n_idx >= ctx.nodes.size()) return;
        const auto& n = ctx.nodes[n_idx];

        if (n.type == NodeType::Block) {
            for (uint32_t i = 0; i < n.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[n.as.block.first_statement + i];
                if (stmt_idx >= ctx.nodes.size()) continue;
                if (ctx.nodes[stmt_idx].type == NodeType::LabelStatement) {
                    uint32_t name_idx = ctx.nodes[stmt_idx].as.label_stmt.name_ident;
                    std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);
                    visible[lname] = stmt_idx;
                }
            }
            for (uint32_t i = 0; i < n.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[n.as.block.first_statement + i];
                if (stmt_idx >= ctx.nodes.size()) continue;
                const auto& stmt = ctx.nodes[stmt_idx];
                if (stmt.type == NodeType::GotoStatement) {
                    uint32_t name_idx = stmt.as.goto_stmt.name_ident;
                    std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);
                    if (visible.count(lname)) {
                        state.goto_targets[stmt_idx] = visible[lname];
                    } else {
                        throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(stmt.line) + ": no visible label '" + std::string(lname) + "' for <goto>");
                    }
                } else {
                    self(self, stmt_idx, visible);
                }
            }
        } else if (n.type == NodeType::FunctionDef) {
            std::map<std::string_view, uint32_t> empty;
            self(self, n.as.func_def.body_block, empty);
        } else if (n.type == NodeType::IfStatement) {
            self(self, n.as.if_stmt.then_block, visible); self(self, n.as.if_stmt.else_block, visible);
        } else if (n.type == NodeType::WhileStatement) { self(self, n.as.while_stmt.body_block, visible);
        } else if (n.type == NodeType::RepeatStatement) { self(self, n.as.repeat_stmt.body_block, visible);
        } else if (n.type == NodeType::ForStatement) { self(self, n.as.for_stmt.body_block, visible);
        } else if (n.type == NodeType::GenericForStatement) { self(self, n.as.generic_for.body_block, visible);
        } else if (n.type == NodeType::DoStatement) { self(self, n.as.do_stmt.body_block, visible);
        } else if (n.type == NodeType::LocalDecl || n.type == NodeType::Assignment || n.type == NodeType::GlobalDeclStatement) {
            uint32_t v_count = (n.type == NodeType::LocalDecl) ? n.as.local_decl.value_count :
                ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count : n.as.assign.value_count);
            uint32_t first_v = (n.type == NodeType::LocalDecl) ? n.as.local_decl.first_value :
                ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value : n.as.assign.first_value);
            for (uint32_t i = 0; i < v_count; ++i) self(self, ctx.block_statements[first_v + i], visible);
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i) self(self, ctx.block_statements[n.as.return_stmt.first_value + i], visible);
        } else if (n.type == NodeType::CallExpression) {
            self(self, n.as.call_expr.target, visible);
            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) self(self, ctx.block_statements[n.as.call_expr.first_arg + i], visible);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2], visible);
                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1], visible);
            }
        } else if (n.type == NodeType::BinaryOp) { self(self, n.as.bin_op.left, visible); self(self, n.as.bin_op.right, visible);
        } else if (n.type == NodeType::UnaryOp) { self(self, n.as.unary_op.expr, visible);
        } else if (n.type == NodeType::ParenExpression) { self(self, n.as.paren_expr.expr, visible); }
    };
    std::map<std::string_view, uint32_t> root_lbls;
    resolve_labels(resolve_labels, root_node, root_lbls);

    state.reassigned_vars.clear();
    std::map<std::string_view, uint32_t> var_assign_counts;
    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement || node.type == NodeType::Assignment) {
            uint32_t t_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count :
                (node.type == NodeType::GlobalDeclStatement ? node.as.global_decl.ident_count : node.as.assign.target_count);
            uint32_t first_t = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident :
                (node.type == NodeType::GlobalDeclStatement ? node.as.global_decl.first_ident : node.as.assign.first_target);
            for (uint32_t i = 0; i < t_count; ++i) {
                uint32_t t_idx = ctx.block_statements[first_t + i];
                if (ctx.nodes[t_idx].type == NodeType::Identifier) {
                    std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    var_assign_counts[name]++;
                }
            }
        }
    }
    for (const auto& pair : var_assign_counts) {
        if (pair.second > 1) state.reassigned_vars.insert(pair.first);
    }

    std::set<std::string_view> for_vars;
    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::ForStatement && node.as.for_stmt.var_ident < ctx.nodes.size()) {
            const auto& vn = ctx.nodes[node.as.for_stmt.var_ident];
            if (vn.type == NodeType::Identifier)
                for_vars.insert(std::string_view(vn.as.ident.name, vn.as.ident.length));
        }
    }
    state.constant_upvalues.clear();
    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::Identifier && node.as.ident.is_captured) {
            std::string_view name(node.as.ident.name, node.as.ident.length);
            if (state.reassigned_vars.count(name) == 0 && for_vars.count(name) == 0)
                state.constant_upvalues.insert(name);
        }
    }

    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::Identifier && (node.as.ident.is_captured || node.as.ident.is_global)) {
            disqualified.insert(std::string_view(node.as.ident.name, node.as.ident.length));
        }
    }

    if (root_node < ctx.nodes.size() && ctx.nodes[root_node].type == NodeType::Block) {
        const ASTNode& root_block = ctx.nodes[root_node];
        for (uint32_t i = 0; i < root_block.as.block.count; ++i) {
            uint32_t stmt_idx = ctx.block_statements[root_block.as.block.first_statement + i];
            const ASTNode& stmt = ctx.nodes[stmt_idx];

            if (stmt.type == NodeType::GlobalDeclStatement && !stmt.as.global_decl.is_wildcard) {
                if (stmt.as.global_decl.ident_count == 1 && stmt.as.global_decl.value_count == 1) {
                    uint32_t id_idx = ctx.block_statements[stmt.as.global_decl.first_ident];
                    uint32_t val_idx = ctx.block_statements[stmt.as.global_decl.first_value];
                    double out_d; int64_t out_i; bool is_int;
                    if (is_literal_number(ctx, val_idx, out_d, out_i, is_int)) {
                        std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                        state.global_constants[name] = out_d;
                    }
                }
            } else if (stmt.type == NodeType::Assignment) {
                if (stmt.as.assign.target_count == 1 && stmt.as.assign.value_count == 1) {
                    uint32_t t_idx = ctx.block_statements[stmt.as.assign.first_target];
                    uint32_t val_idx = ctx.block_statements[stmt.as.assign.first_value];
                    if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[t_idx].as.ident.is_global) {
                        double out_d; int64_t out_i; bool is_int;
                        if (is_literal_number(ctx, val_idx, out_d, out_i, is_int)) {
                            std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                            state.global_constants[name] = out_d;
                        }
                    }
                }
            }
        }
    }

    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::Block) {
            std::map<uint32_t, uint32_t> pending_tables;

            for (uint32_t i = 0; i < node.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
                const ASTNode& stmt = ctx.nodes[stmt_idx];

                if (stmt.type == NodeType::LocalDecl && stmt.as.local_decl.value_count == 1) {
                    uint32_t val_idx = ctx.block_statements[stmt.as.local_decl.first_value];
                    if (ctx.nodes[val_idx].type == NodeType::TableConstructor && ctx.nodes[val_idx].as.table_cons.count == 0) {
                        pending_tables[ctx.block_statements[stmt.as.local_decl.first_ident]] = val_idx;
                    }
                } else if (stmt.type == NodeType::Assignment && stmt.as.assign.value_count == 1) {
                    uint32_t val_idx = ctx.block_statements[stmt.as.assign.first_value];
                    if (ctx.nodes[val_idx].type == NodeType::TableConstructor && ctx.nodes[val_idx].as.table_cons.count == 0) {
                        pending_tables[ctx.block_statements[stmt.as.assign.first_target]] = val_idx;
                    }
                 } else if (stmt.type == NodeType::ForStatement) {
                     auto has_forward_ref = [&](uint32_t expr_node, const std::vector<std::string>& pending_names) -> bool {
                         std::vector<uint32_t> stack = {expr_node};
                         while (!stack.empty()) {
                             uint32_t nid = stack.back(); stack.pop_back();
                             const auto& n = ctx.nodes[nid];
                             if (n.type == NodeType::Identifier && !n.as.ident.is_global) {
                                 std::string_view nm(n.as.ident.name, n.as.ident.length);
                                 for (auto& pn : pending_names) {
                                     if (nm == pn) return true;
                                 }
                                 continue;
                             }
                             auto push = [&](uint32_t child) { if (child != 0xFFFFFFFF && child < ctx.nodes.size()) stack.push_back(child); };
                              switch (n.type) {
                                  case NodeType::BinaryOp: push(n.as.bin_op.left); push(n.as.bin_op.right); break;
                                  case NodeType::UnaryOp: push(n.as.unary_op.expr); break;
                                  case NodeType::ParenExpression: push(n.as.paren_expr.expr); break;
                                  case NodeType::CallExpression: { for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) push(ctx.block_statements[n.as.call_expr.first_arg + i]); push(n.as.call_expr.target); break; }
                                  case NodeType::TableAccess: push(n.as.table_access.table); push(n.as.table_access.key); break;
                                  case NodeType::IntrinsicCall: { for (uint32_t i = 0; i < n.as.intrinsic_call.arg_count; ++i) push(ctx.block_statements[n.as.intrinsic_call.first_arg + i]); break; }
                                 default: break;
                             }
                         }
                         return false;
                     };
                     std::vector<std::string> _pending_names;
                     for (auto& _pp : pending_tables) {
                         _pending_names.push_back(get_ast_string(ctx, _pp.first));
                     }
                     for (auto& pair : pending_tables) {
                         uint32_t table_node = pair.second;
                         if (state.table_presize.find(table_node) == state.table_presize.end()) {
                             if (!has_forward_ref(stmt.as.for_stmt.limit_expr, _pending_names)) {
                                 state.table_presize[table_node] = stmt.as.for_stmt.limit_expr;
                                 std::string t_str = get_ast_string(ctx, pair.first);
                                 if (!t_str.empty()) array_bounds[t_str] = stmt.as.for_stmt.limit_expr;
                             }
                         }
                     }
                     pending_tables.clear();
                 }
            }
        }

        if (node.type == NodeType::ForStatement) {
            std::string_view name(ctx.nodes[node.as.for_stmt.var_ident].as.ident.name, ctx.nodes[node.as.for_stmt.var_ident].as.ident.length);
            known_numbers.insert(name);

            std::string vname = get_ast_string(ctx, node.as.for_stmt.var_ident);
            if (!vname.empty()) {
                if (loop_limits.count(vname) && loop_limits[vname] != node.as.for_stmt.limit_expr) {
                    std::string l1 = get_ast_string(ctx, loop_limits[vname]);
                    std::string l2 = get_ast_string(ctx, node.as.for_stmt.limit_expr);
                    if (l1 != l2) loop_limit_conflicts[vname] = true;
                }
                loop_limits[vname] = node.as.for_stmt.limit_expr;
            }
        }
    }

    std::vector<std::pair<std::string_view, uint32_t>> func_defs;
    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement) {
            uint32_t i_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count : node.as.global_decl.ident_count;
            uint32_t f_ident = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident : node.as.global_decl.first_ident;
            uint32_t f_value = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value : node.as.global_decl.first_value;
            uint32_t v_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count : node.as.global_decl.value_count;

            if (i_count == 1 && v_count == 1) {
                uint32_t t_idx = ctx.block_statements[f_ident];
                uint32_t v_idx = ctx.block_statements[f_value];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[v_idx].type == NodeType::FunctionDef) {
                    std::string_view fname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    if (state.reassigned_vars.count(fname) == 0) {
                        state.func_param_counts[fname] = ctx.nodes[v_idx].as.func_def.param_count;
                        state.func_param_native[fname] = std::vector<bool>(ctx.nodes[v_idx].as.func_def.param_count, true);
                        func_defs.push_back({fname, v_idx});
                    }
                }
            }
        } else if (node.type == NodeType::Assignment) {
            if (node.as.assign.target_count == 1 && node.as.assign.value_count == 1) {
                uint32_t t_idx = ctx.block_statements[node.as.assign.first_target];
                uint32_t v_idx = ctx.block_statements[node.as.assign.first_value];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[v_idx].type == NodeType::FunctionDef) {
                    std::string_view fname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    if (state.reassigned_vars.count(fname) == 0) {
                        state.func_param_counts[fname] = ctx.nodes[v_idx].as.func_def.param_count;
                        state.func_param_native[fname] = std::vector<bool>(ctx.nodes[v_idx].as.func_def.param_count, true);
                        func_defs.push_back({fname, v_idx});
                    }
                }
            }
        }
    }

    auto traverse_node = [&](auto& self, uint32_t node_idx, auto& cb) -> void {
        if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size()) return;
        cb(node_idx);
        const auto& n = ctx.nodes[node_idx];
        if (n.type == NodeType::Block) {
            for (uint32_t i = 0; i < n.as.block.count; ++i) self(self, ctx.block_statements[n.as.block.first_statement + i], cb);
        } else if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement || n.type == NodeType::Assignment) {
            uint32_t v_count = (n.type == NodeType::LocalDecl) ? n.as.local_decl.value_count :
                ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count : n.as.assign.value_count);
            uint32_t f_value = (n.type == NodeType::LocalDecl) ? n.as.local_decl.first_value :
                ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value : n.as.assign.first_value);
            for (uint32_t i = 0; i < v_count; ++i) self(self, ctx.block_statements[f_value + i], cb);
        } else if (n.type == NodeType::BinaryOp) {
            self(self, n.as.bin_op.left, cb); self(self, n.as.bin_op.right, cb);
        } else if (n.type == NodeType::UnaryOp) {
            self(self, n.as.unary_op.expr, cb);
        } else if (n.type == NodeType::CallExpression) {
            self(self, n.as.call_expr.target, cb);
            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) self(self, ctx.block_statements[n.as.call_expr.first_arg + i], cb);
        } else if (n.type == NodeType::IfStatement) {
            self(self, n.as.if_stmt.condition, cb); self(self, n.as.if_stmt.then_block, cb); self(self, n.as.if_stmt.else_block, cb);
        } else if (n.type == NodeType::WhileStatement) {
            self(self, n.as.while_stmt.condition, cb); self(self, n.as.while_stmt.body_block, cb);
        } else if (n.type == NodeType::RepeatStatement) {
            self(self, n.as.repeat_stmt.body_block, cb); self(self, n.as.repeat_stmt.condition, cb);
        } else if (n.type == NodeType::ForStatement) {
            self(self, n.as.for_stmt.start_expr, cb); self(self, n.as.for_stmt.limit_expr, cb);
            self(self, n.as.for_stmt.step_expr, cb); self(self, n.as.for_stmt.body_block, cb);
        } else if (n.type == NodeType::GenericForStatement) {
            for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                self(self, ctx.block_statements[n.as.generic_for.first_iter + i], cb);
            self(self, n.as.generic_for.body_block, cb);
        } else if (n.type == NodeType::DoStatement) {
            self(self, n.as.do_stmt.body_block, cb);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2], cb);
                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1], cb);
            }
        } else if (n.type == NodeType::TableAccess) {
            self(self, n.as.table_access.table, cb); self(self, n.as.table_access.key, cb);
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
                self(self, ctx.block_statements[n.as.return_stmt.first_value + i], cb);
        } else if (n.type == NodeType::FunctionDef) {
            self(self, n.as.func_def.body_block, cb);
        } else if (n.type == NodeType::ParenExpression) {
            self(self, n.as.paren_expr.expr, cb);
        }
    };

    std::map<uint32_t, std::string_view> call_to_func;
    for (const auto& fdef : func_defs) {
        auto cb = [&](uint32_t idx) {
            if (ctx.nodes[idx].type == NodeType::CallExpression) call_to_func[idx] = fdef.first;
        };
        traverse_node(traverse_node, ctx.nodes[fdef.second].as.func_def.body_block, cb);
    }

    std::set<std::string_view> escaped_funcs;
    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto& n = ctx.nodes[i];

        auto check_escape = [&](uint32_t idx) {
            while (idx != 0xFFFFFFFF && idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::ParenExpression) {
                idx = ctx.nodes[idx].as.paren_expr.expr;
            }
            if (idx != 0xFFFFFFFF && idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::Identifier) {
                escaped_funcs.insert(std::string_view(ctx.nodes[idx].as.ident.name, ctx.nodes[idx].as.ident.length));
            }
        };

        if (n.type == NodeType::CallExpression) {
            for (uint32_t a = 0; a < n.as.call_expr.arg_count; ++a) check_escape(ctx.block_statements[n.as.call_expr.first_arg + a]);
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t a = 0; a < n.as.return_stmt.value_count; ++a) check_escape(ctx.block_statements[n.as.return_stmt.first_value + a]);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t a = 0; a < n.as.table_cons.count; ++a) check_escape(ctx.block_statements[n.as.table_cons.first_item + a * 2 + 1]);
        } else if (n.type == NodeType::Assignment) {
            for (uint32_t a = 0; a < n.as.assign.value_count; ++a) check_escape(ctx.block_statements[n.as.assign.first_value + a]);
        } else if (n.type == NodeType::LocalDecl) {
            for (uint32_t a = 0; a < n.as.local_decl.value_count; ++a) check_escape(ctx.block_statements[n.as.local_decl.first_value + a]);
        } else if (n.type == NodeType::GenericForStatement) {
            for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                check_escape(ctx.block_statements[n.as.generic_for.first_iter + i]);
        } else if (n.type == NodeType::TableAccess) {
            check_escape(n.as.table_access.key);
        } else if (n.type == NodeType::BinaryOp) {
            check_escape(n.as.bin_op.left);
            check_escape(n.as.bin_op.right);
        } else if (n.type == NodeType::UnaryOp) {
            check_escape(n.as.unary_op.expr);
        }
    }

    for (auto fname : escaped_funcs) {
        if (state.func_param_native.count(fname)) {
            for (size_t p = 0; p < state.func_param_native[fname].size(); ++p) {
                state.func_param_native[fname][p] = false;
            }
        }
    }

    for (auto fname : escaped_funcs) {
        for (const auto& fdef : func_defs) {
            if (fdef.first == fname) {
                uint32_t f_idx = fdef.second;
                const auto& fn = ctx.nodes[f_idx];
                for (size_t p = 0; p < fn.as.func_def.param_count; ++p) {
                    uint32_t p_idx = ctx.block_statements[fn.as.func_def.first_param + p];
                    std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                    disqualified.insert(pname);
                    known_numbers.erase(pname);
                }
            }
        }
    }

    bool changed;
    int safety_limit = 100;
    do {
        changed = false;
        if (--safety_limit <= 0) break;

        for (const auto& node : ctx.nodes) {
            if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement) {
                if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard) continue;
                uint32_t i_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count : node.as.global_decl.ident_count;
                uint32_t f_ident = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident : node.as.global_decl.first_ident;
                uint32_t f_value = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value : node.as.global_decl.first_value;
                uint32_t v_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count : node.as.global_decl.value_count;

                for (uint32_t i = 0; i < i_count; ++i) {
                    uint32_t id_idx = ctx.block_statements[f_ident + i];
                    std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                    uint32_t val_idx = (i < v_count) ? ctx.block_statements[f_value + i] : 0xFFFFFFFF;

                    if (disqualified.count(name)) {
                        if (known_numbers.erase(name)) changed = true;
                    } else if (ctx.nodes[id_idx].as.ident.is_captured || ctx.nodes[id_idx].as.ident.is_global) {
                        disqualified.insert(name);
                        if (known_numbers.erase(name)) changed = true;
                    } else if (yields_number(ctx, state, val_idx, &known_numbers)) {
                        if (known_numbers.insert(name).second) changed = true;
                    } else {
                        disqualified.insert(name);
                        if (known_numbers.erase(name)) changed = true;
                    }
                }
            } else if (node.type == NodeType::Assignment) {
                for (uint32_t i = 0; i < node.as.assign.target_count; ++i) {
                    uint32_t t_idx = ctx.block_statements[node.as.assign.first_target + i];
                    const auto& t_node = ctx.nodes[t_idx];
                    if (t_node.type == NodeType::Identifier) {
                        std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
                        uint32_t val_idx = (i < node.as.assign.value_count) ? ctx.block_statements[node.as.assign.first_value + i] : 0xFFFFFFFF;

                        if (disqualified.count(name)) {
                            if (known_numbers.erase(name)) changed = true;
                        } else if (t_node.as.ident.is_global) {
                            disqualified.insert(name);
                            if (known_numbers.erase(name)) changed = true;
                        } else if (yields_number(ctx, state, val_idx, &known_numbers)) {
                            if (!disqualified.count(name) && known_numbers.insert(name).second) changed = true;
                        } else {
                            disqualified.insert(name);
                            if (known_numbers.erase(name)) changed = true;
                        }
                    }
                }
            } else if (node.type == NodeType::GenericForStatement) {
                for (uint32_t i = 0; i < node.as.generic_for.var_count; ++i) {
                    uint32_t v_idx = ctx.block_statements[node.as.generic_for.first_var + i];
                    if (ctx.nodes[v_idx].type == NodeType::Identifier) {
                        std::string_view name(ctx.nodes[v_idx].as.ident.name, ctx.nodes[v_idx].as.ident.length);
                        if (disqualified.insert(name).second) {
                            known_numbers.erase(name);
                            changed = true;
                        }
                    }
                }
            }
        }
    } while (changed);

    for (auto& [fname, is_native_vec] : state.func_param_native) {
        for (const auto& fdef : func_defs) {
            if (fdef.first == fname) {
                uint32_t f_idx = fdef.second;
                const auto& fn = ctx.nodes[f_idx];
                for (size_t p = 0; p < fn.as.func_def.param_count && p < is_native_vec.size(); ++p) {
                    uint32_t p_idx = ctx.block_statements[fn.as.func_def.first_param + p];
                    if (ctx.nodes[p_idx].type == NodeType::Identifier) {
                        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                        if (disqualified.find(pname) != disqualified.end()) {
                            is_native_vec[p] = false;
                        }
                    }
                }
            }
        }
    }

    for (auto& [fname, is_native_vec] : state.func_param_native) {
        for (const auto& fdef : func_defs) {
            if (fdef.first != fname) continue;
            const auto& fn = ctx.nodes[fdef.second];
            std::set<std::string_view> pnames;
            for (size_t p = 0; p < fn.as.func_def.param_count && p < is_native_vec.size(); ++p) {
                uint32_t pi = ctx.block_statements[fn.as.func_def.first_param + p];
                if (ctx.nodes[pi].type == NodeType::Identifier)
                    pnames.insert(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
            }
            if (pnames.empty()) continue;
            auto nil_scan = [&](auto& self, uint32_t nidx) -> void {
                if (nidx >= ctx.nodes.size()) return;
                const auto& n = ctx.nodes[nidx];
                if (n.type == NodeType::BinaryOp && n.as.bin_op.op == static_cast<int>(BinaryOp::Eq)) {
                    auto try_disqualify = [&](uint32_t a, uint32_t b) {
                        if (ctx.nodes[a].type != NodeType::Identifier || ctx.nodes[b].type != NodeType::NilLiteral) return;
                        std::string_view nm(ctx.nodes[a].as.ident.name, ctx.nodes[a].as.ident.length);
                        if (!pnames.count(nm)) return;
                        for (size_t p = 0; p < fn.as.func_def.param_count; ++p) {
                            uint32_t pi = ctx.block_statements[fn.as.func_def.first_param + p];
                            if (ctx.nodes[pi].type == NodeType::Identifier &&
                                std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length) == nm)
                                { is_native_vec[p] = false; break; }
                        }
                    };
                    try_disqualify(n.as.bin_op.left, n.as.bin_op.right);
                    try_disqualify(n.as.bin_op.right, n.as.bin_op.left);
                }
                if (n.type == NodeType::Block)
                    for (uint32_t i = 0; i < n.as.block.count; ++i)
                        self(self, ctx.block_statements[n.as.block.first_statement + i]);
            };
            nil_scan(nil_scan, fn.as.func_def.body_block);
        }
    }

    bool params_changed;
    do {
        params_changed = false;
        for (uint32_t n_idx = 0; n_idx < ctx.nodes.size(); ++n_idx) {
            const auto& node = ctx.nodes[n_idx];
            if (node.type == NodeType::CallExpression) {
                uint32_t tgt = node.as.call_expr.target;
                if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                    std::string_view fname(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                    if (state.func_param_native.count(fname)) {
                        std::string_view current_func = call_to_func.count(n_idx) ? call_to_func[n_idx] : "";
                        std::set<std::string_view> current_params;
                        if (!current_func.empty() && state.func_param_native.count(current_func)) {
                            uint32_t c_fdef_idx = 0xFFFFFFFF;
                            for (auto fd : func_defs) if (fd.first == current_func) c_fdef_idx = fd.second;
                            if (c_fdef_idx != 0xFFFFFFFF) {
                                const auto& fn = ctx.nodes[c_fdef_idx];
                                for (size_t p = 0; p < fn.as.func_def.param_count; ++p) {
                                    if (state.func_param_native[current_func][p]) {
                                        uint32_t p_idx = ctx.block_statements[fn.as.func_def.first_param + p];
                                        current_params.insert(std::string_view(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length));
                                    }
                                }
                            }
                        }

                        for (size_t p = 0; p < state.func_param_counts[fname]; ++p) {
                            if (state.func_param_native[fname][p]) {
                                if (p < node.as.call_expr.arg_count) {
                                    uint32_t arg_idx = ctx.block_statements[node.as.call_expr.first_arg + p];
                                    if (!yields_number(ctx, state, arg_idx, &known_numbers, current_func, &current_params)) {
                                        state.func_param_native[fname][p] = false;
                                        params_changed = true;
                                    }
                                } else {
                                    state.func_param_native[fname][p] = false;
                                    params_changed = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    } while (params_changed);

    for (const auto& fdef : func_defs) {
        if (function_returns_native(ctx, state, fdef.second, fdef.first, &known_numbers)) {
            state.native_return_funcs.insert(fdef.first);
        }
        if (state.func_param_native.count(fdef.first)) {
            const auto& fn = ctx.nodes[fdef.second].as.func_def;
            for (size_t p = 0; p < fn.param_count; ++p) {
                if (state.func_param_native[fdef.first][p]) {
                    uint32_t p_idx = ctx.block_statements[fn.first_param + p];
                    std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                    if (!ctx.nodes[p_idx].as.ident.is_captured) {
                        known_numbers.insert(pname);
                    }
                }
            }
        }
    }

    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        if (ctx.nodes[i].type == NodeType::TableAccess) {
            std::string t_str = get_ast_string(ctx, ctx.nodes[i].as.table_access.table);
            uint32_t base_k_idx = ctx.nodes[i].as.table_access.key;
            if (ctx.nodes[base_k_idx].type == NodeType::BinaryOp) {
                int op = ctx.nodes[base_k_idx].as.bin_op.op;
                if (op == static_cast<int>(BinaryOp::Add) || op == static_cast<int>(BinaryOp::Sub)) {
                    base_k_idx = ctx.nodes[base_k_idx].as.bin_op.left;
                }
            }
            std::string k_str = get_ast_string(ctx, base_k_idx);

            if (!t_str.empty() && !k_str.empty() && array_bounds.count(t_str) && loop_limits.count(k_str) && !loop_limit_conflicts[k_str]) {
                uint32_t bound_expr = array_bounds[t_str];
                uint32_t loop_expr = loop_limits[k_str];

                if (bound_expr == loop_expr) {
                    state.bce_safe_nodes.insert(i);
                } else {
                    std::string b_str = get_ast_string(ctx, bound_expr);
                    std::string l_str = get_ast_string(ctx, loop_expr);
                    if (!b_str.empty() && !l_str.empty()) {
                        if (b_str == l_str) state.bce_safe_nodes.insert(i);
                        else if (l_str.find(b_str + " -") == 0) state.bce_safe_nodes.insert(i);
                    } else {
                        double bd=0, ld=0; int64_t bi=0, li=0; bool bint, lint;
                        if (is_literal_number(ctx, bound_expr, bd, bi, bint) && is_literal_number(ctx, loop_expr, ld, li, lint)) {
                            if (bd >= ld) state.bce_safe_nodes.insert(i);
                        }
                    }
                }
            }
        }
    }

    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::String) {
            std::string_view s(node.as.string.text, node.as.string.length);
            if (std::find(state.string_pool.begin(), state.string_pool.end(), s) == state.string_pool.end()) state.string_pool.push_back(s);
        }
        if (node.type == NodeType::Identifier) {
            std::string_view s(node.as.ident.name, node.as.ident.length);
            if (std::find(state.string_pool.begin(), state.string_pool.end(), s) == state.string_pool.end()) state.string_pool.push_back(s);
        }
    }

    state.pure_numeric_arrays.clear();
    std::set<std::string_view> pure_candidates;
    std::set<std::string_view> disqualified_arrays;

    auto purity_cb = [&](uint32_t idx) {
        if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size()) return;
        const auto& n = ctx.nodes[idx];
        if (n.type == NodeType::LocalDecl) {
            if (n.as.local_decl.ident_count == 1 && n.as.local_decl.value_count == 1) {
                uint32_t t_idx = ctx.block_statements[n.as.local_decl.first_ident];
                uint32_t v_idx = ctx.block_statements[n.as.local_decl.first_value];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[v_idx].type == NodeType::TableConstructor) {
                    std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    const auto& tc = ctx.nodes[v_idx].as.table_cons;
                    // Record known length for tables with fixed constructors
                    if (tc.count > 0 && state.known_table_lengths.find(name) == state.known_table_lengths.end()) {
                        state.known_table_lengths[name] = tc.count;
                    }
                    if (tc.count > 0) {
                        bool has_string_key = false;
                        bool all_numeric = true;
                        for (uint32_t ei = 0; ei < tc.count && !has_string_key && all_numeric; ++ei) {
                            uint32_t ek = ctx.block_statements[tc.first_item + ei * 2];
                            uint32_t ev = ctx.block_statements[tc.first_item + ei * 2 + 1];
                            if (ek < ctx.nodes.size() && ctx.nodes[ek].type == NodeType::String) {
                                has_string_key = true;
                            }
                            if (ev >= ctx.nodes.size() || !yields_number(ctx, state, ev, &known_numbers)) {
                                all_numeric = false;
                            }
                        }
                        if (!has_string_key && all_numeric) {
                            pure_candidates.insert(name);
                        } else {
                            disqualified_arrays.insert(name);
                        }
                    } else {
                        pure_candidates.insert(name);
                    }
                }
            }
            for (uint32_t i = 0; i < n.as.local_decl.value_count; ++i) {
                uint32_t v = ctx.block_statements[n.as.local_decl.first_value + i];
                if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::Identifier && !yields_number(ctx, state, v, &known_numbers)) {
                    std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
        } else if (n.type == NodeType::Assignment) {
            for (uint32_t i = 0; i < n.as.assign.value_count; ++i) {
                uint32_t v = ctx.block_statements[n.as.assign.first_value + i];
                if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::Identifier && !yields_number(ctx, state, v, &known_numbers)) {
                    std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
            if (n.as.assign.target_count == 1) {
                uint32_t t_idx = ctx.block_statements[n.as.assign.first_target];
                if (ctx.nodes[t_idx].type == NodeType::TableAccess) {
                    uint32_t tb_idx = ctx.nodes[t_idx].as.table_access.table;
                    if (ctx.nodes[tb_idx].type == NodeType::Identifier) {
                        std::string_view tname(ctx.nodes[tb_idx].as.ident.name, ctx.nodes[tb_idx].as.ident.length);

                        uint32_t k_idx = ctx.nodes[t_idx].as.table_access.key;
                        bool key_ok = yields_number(ctx, state, k_idx, &known_numbers);

                        if (!key_ok) {

                            std::unordered_set<uint32_t> _opt_visited;
                            auto key_depends_on_self = [&](auto& self, uint32_t nid) -> bool {
                                if (nid >= ctx.nodes.size() || _opt_visited.count(nid)) return false;
                                _opt_visited.insert(nid);
                                const auto& nn = ctx.nodes[nid];
                                if (nn.type == NodeType::TableAccess) {
                                    uint32_t stbl = nn.as.table_access.table;
                                    if (stbl < ctx.nodes.size() && ctx.nodes[stbl].type == NodeType::Identifier &&
                                        std::string_view(ctx.nodes[stbl].as.ident.name, ctx.nodes[stbl].as.ident.length) == tname)
                                        return true;
                                }
                                if (nn.type == NodeType::Identifier) {

                                    std::string_view vn(nn.as.ident.name, nn.as.ident.length);
                                    for (const auto& nd : ctx.nodes) {
                                        if (nd.type != NodeType::Assignment) continue;
                                        if (nd.as.assign.target_count != 1) continue;
                                        uint32_t ti = ctx.block_statements[nd.as.assign.first_target];
                                        if (ctx.nodes[ti].type != NodeType::Identifier) continue;
                                        if (std::string_view(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length) != vn) continue;
                                        uint32_t vi = ctx.block_statements[nd.as.assign.first_value];
                                        return self(self, vi);
                                    }
                                    for (const auto& nd : ctx.nodes) {
                                        if (nd.type != NodeType::LocalDecl) continue;
                                        for (uint32_t ii = 0; ii < nd.as.local_decl.ident_count; ++ii) {
                                            uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + ii];
                                            if (ctx.nodes[idi].type != NodeType::Identifier) continue;
                                            if (std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length) != vn) continue;
                                            uint32_t vi = (ii < nd.as.local_decl.value_count)
                                                ? ctx.block_statements[nd.as.local_decl.first_value + ii] : 0xFFFFFFFF;
                                            if (vi != 0xFFFFFFFF) return self(self, vi);
                                            break;
                                        }
                                    }
                                    return false;
                                }
                                if (nn.type == NodeType::BinaryOp)
                                    return self(self, nn.as.bin_op.left) || self(self, nn.as.bin_op.right);
                                if (nn.type == NodeType::UnaryOp)
                                    return self(self, nn.as.unary_op.expr);
                                return false;
                            };
                            if (!key_depends_on_self(key_depends_on_self, k_idx)) {
                                disqualified_arrays.insert(tname);
                            }
                        } else {
                            // Key is numeric — but only simple identifiers and literals
                            // qualify for the vector path. Expressions like i*1000 or i+1
                            // produce sparse keys that would blow up the vector.
                            {
                                NodeType ktype = ctx.nodes[k_idx].type;
                                if (ktype != NodeType::Identifier && ktype != NodeType::Number && ktype != NodeType::Integer) {
                                    disqualified_arrays.insert(tname);
                                }
                            }
                            uint32_t v_idx = ctx.block_statements[n.as.assign.first_value];
                        if (!yields_number(ctx, state, v_idx, &known_numbers) && ctx.nodes[v_idx].type != NodeType::TrueLiteral &&
                            ctx.nodes[v_idx].type != NodeType::FalseLiteral && ctx.nodes[v_idx].type != NodeType::TableAccess) {
                            disqualified_arrays.insert(tname);
                            }
                        }
                    }
                }
            }
        } else if (n.type == NodeType::CallExpression) {
            // Detect setmetatable(t, mt) / table.insert(t, ...) / table.remove(t, ...)
            // — mark first arg as having dynamic length
            uint32_t _call_target = n.as.call_expr.target;
            if (_call_target < ctx.nodes.size() && n.as.call_expr.arg_count >= 1) {
                uint32_t _arg0 = ctx.block_statements[n.as.call_expr.first_arg];
                if (_arg0 < ctx.nodes.size() && ctx.nodes[_arg0].type == NodeType::Identifier) {
                    std::string_view _arg_name(ctx.nodes[_arg0].as.ident.name, ctx.nodes[_arg0].as.ident.length);
                    bool _is_dynamic = false;
                    if (ctx.nodes[_call_target].type == NodeType::Identifier) {
                        std::string_view _fname(ctx.nodes[_call_target].as.ident.name, ctx.nodes[_call_target].as.ident.length);
                        _is_dynamic = (_fname == "setmetatable");
                    } else if (ctx.nodes[_call_target].type == NodeType::TableAccess) {
                        uint32_t _tbl = ctx.nodes[_call_target].as.table_access.table;
                        uint32_t _key = ctx.nodes[_call_target].as.table_access.key;
                        if (_tbl < ctx.nodes.size() && ctx.nodes[_tbl].type == NodeType::Identifier &&
                            _key < ctx.nodes.size() && ctx.nodes[_key].type == NodeType::String) {
                            std::string_view _tbl_n(ctx.nodes[_tbl].as.ident.name, ctx.nodes[_tbl].as.ident.length);
                            std::string_view _key_n(ctx.nodes[_key].as.string.text, ctx.nodes[_key].as.string.length);
                            _is_dynamic = (_tbl_n == "table" && (_key_n == "insert" || _key_n == "remove"));
                        }
                    }
                    if (_is_dynamic) state.tables_with_dynamic_length.insert(_arg_name);
                }
            }

            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) {
                uint32_t v = ctx.block_statements[n.as.call_expr.first_arg + i];
                if (v < ctx.nodes.size()) {
                    if (ctx.nodes[v].type == NodeType::Identifier) {
                        std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                        disqualified_arrays.insert(name);
                    } else if (ctx.nodes[v].type == NodeType::TableConstructor) {
                        const auto& tc = ctx.nodes[v].as.table_cons;
                        for (uint32_t ei = 0; ei < tc.count; ++ei) {
                            uint32_t ev = ctx.block_statements[tc.first_item + ei * 2 + 1];
                            if (ev < ctx.nodes.size() && ctx.nodes[ev].type == NodeType::Identifier) {
                                std::string_view name(ctx.nodes[ev].as.ident.name, ctx.nodes[ev].as.ident.length);
                                disqualified_arrays.insert(name);
                            }
                        }
                    }
                }
            }
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i) {
                uint32_t v = ctx.block_statements[n.as.return_stmt.first_value + i];
                if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::Identifier) {
                    std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
        } else if (n.type == NodeType::GenericForStatement) {
            for (uint32_t i = 0; i < n.as.generic_for.var_count; ++i) {
                uint32_t v_idx = ctx.block_statements[n.as.generic_for.first_var + i];
                if (v_idx < ctx.nodes.size() && ctx.nodes[v_idx].type == NodeType::Identifier) {
                    std::string_view name(ctx.nodes[v_idx].as.ident.name, ctx.nodes[v_idx].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
        } else if (n.type == NodeType::TableAccess) {
            uint32_t k_idx = n.as.table_access.key;
            if (k_idx < ctx.nodes.size() && ctx.nodes[k_idx].type == NodeType::Identifier) {
                std::string_view name(ctx.nodes[k_idx].as.ident.name, ctx.nodes[k_idx].as.ident.length);
                disqualified_arrays.insert(name);
            }
        } else if (n.type == NodeType::UnaryOp) {
            if (n.as.unary_op.op == static_cast<int>(UnaryOp::Len)) {
                uint32_t v = n.as.unary_op.expr;
                if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::Identifier) {
                    std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
        }
    };
    traverse_node(traverse_node, root_node, purity_cb);

    for (auto name : pure_candidates) {
        if (disqualified_arrays.find(name) == disqualified_arrays.end()) {
            state.pure_numeric_arrays.insert(name);
        }
    }

    state.numeric_table_fields.clear();
    for (const auto& node : ctx.nodes) {
        if (node.type != NodeType::LocalDecl) continue;
        if (node.as.local_decl.ident_count != 1 || node.as.local_decl.value_count != 1) continue;
        uint32_t id_idx = ctx.block_statements[node.as.local_decl.first_ident];
        uint32_t val_idx = ctx.block_statements[node.as.local_decl.first_value];
        if (ctx.nodes[id_idx].type != NodeType::Identifier) continue;
        std::string_view nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
        if (ctx.nodes[val_idx].type != NodeType::TableConstructor) continue;
        const auto& tc = ctx.nodes[val_idx].as.table_cons;
        if (tc.count == 0) continue;

        std::map<std::string_view, bool> field_numeric;
        std::set<std::string_view> all_fields;
        bool first = true;
        bool valid = true;
        for (uint32_t ei = 0; ei < tc.count && valid; ++ei) {
            uint32_t ev = ctx.block_statements[tc.first_item + ei * 2 + 1];
            if (ctx.nodes[ev].type != NodeType::TableConstructor) { valid = false; break; }
            const auto& inner = ctx.nodes[ev].as.table_cons;
            std::set<std::string_view> entry_fields;
            for (uint32_t fi = 0; fi < inner.count; ++fi) {
                uint32_t fk = ctx.block_statements[inner.first_item + fi * 2];
                uint32_t fv = ctx.block_statements[inner.first_item + fi * 2 + 1];
                if (fk >= ctx.nodes.size() || ctx.nodes[fk].type != NodeType::String) continue;
                std::string_view fname(ctx.nodes[fk].as.string.text, ctx.nodes[fk].as.string.length);
                entry_fields.insert(fname);
                bool is_num = yields_number(ctx, state, fv, nullptr);
                if (!is_num && ctx.nodes[fv].type == NodeType::Identifier) {

                    std::string_view vname(ctx.nodes[fv].as.ident.name, ctx.nodes[fv].as.ident.length);
                    for (const auto& nd : ctx.nodes) {
                        if (nd.type != NodeType::LocalDecl) continue;
                        for (uint32_t ii = 0; ii < nd.as.local_decl.ident_count; ++ii) {
                            uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + ii];
                            if (ctx.nodes[idi].type != NodeType::Identifier) continue;
                            if (std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length) != vname) continue;
                            uint32_t vi = (ii < nd.as.local_decl.value_count)
                                ? ctx.block_statements[nd.as.local_decl.first_value + ii] : 0xFFFFFFFF;
                            if (vi != 0xFFFFFFFF && yields_number(ctx, state, vi, &known_numbers)) { is_num = true; break; }
                        }
                        if (is_num) break;
                    }
                }
                if (first) field_numeric[fname] = is_num;
                else if (field_numeric.count(fname) && field_numeric[fname] && !is_num)
                    field_numeric[fname] = false;
            }
            if (first) all_fields = entry_fields;
            else {
                std::set<std::string_view> intersect;
                for (auto& f : all_fields) if (entry_fields.count(f)) intersect.insert(f);
                all_fields = intersect;
            }
            first = false;
        }
        if (valid && !all_fields.empty()) {
            std::set<std::string_view> numeric_fields;
            for (auto& f : all_fields) {
                auto it = field_numeric.find(f);
                if (it != field_numeric.end() && it->second) numeric_fields.insert(it->first);
            }
            if (!numeric_fields.empty()) {
                state.numeric_table_fields[nm] = numeric_fields;

                for (auto& fld : numeric_fields) {
                    if (std::find(state.string_pool.begin(), state.string_pool.end(), fld) == state.string_pool.end())
                        state.string_pool.push_back(fld);
                }
            }
        }
    }

    for (const auto& node : ctx.nodes) {
        if (node.type != NodeType::LocalDecl) continue;
        if (node.as.local_decl.ident_count != 1 || node.as.local_decl.value_count != 1) continue;
        uint32_t id_idx = ctx.block_statements[node.as.local_decl.first_ident];
        uint32_t val_idx = ctx.block_statements[node.as.local_decl.first_value];
        if (id_idx >= ctx.nodes.size() || val_idx >= ctx.nodes.size()) continue;
        if (ctx.nodes[id_idx].type != NodeType::Identifier) continue;
        std::string_view nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
        if (ctx.nodes[val_idx].type != NodeType::TableConstructor) continue;
        if (state.numeric_table_fields.count(nm)) continue;
        const auto& tc = ctx.nodes[val_idx].as.table_cons;
        if (tc.count == 0) continue;

        std::set<std::string_view> numeric_fields;
        bool valid = true;
        for (uint32_t ei = 0; ei < tc.count && valid; ++ei) {
            uint32_t fk = ctx.block_statements[tc.first_item + ei * 2];
            uint32_t fv = ctx.block_statements[tc.first_item + ei * 2 + 1];
            if (fk >= ctx.nodes.size() || fv >= ctx.nodes.size()) { valid = false; break; }
            if (ctx.nodes[fk].type == NodeType::String) {
                std::string_view fname(ctx.nodes[fk].as.string.text, ctx.nodes[fk].as.string.length);
                if (yields_number(ctx, state, fv, &known_numbers))
                    numeric_fields.insert(fname);
            } else if (!yields_number(ctx, state, fv, &known_numbers)) {
                valid = false;
            }
        }

        if (valid && !numeric_fields.empty()) {
            state.numeric_table_fields[nm] = numeric_fields;
            for (auto& fld : numeric_fields) {
                if (std::find(state.string_pool.begin(), state.string_pool.end(), fld) == state.string_pool.end())
                    state.string_pool.push_back(fld);
            }
        }
    }
    for (const auto& nd : ctx.nodes) {
        if (nd.type != NodeType::FunctionDef) continue;
        std::set<std::string_view> func_params;
        for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
            uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
            if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier) {
                func_params.insert(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
            }
        }
        if (func_params.empty()) continue;
        std::map<std::string_view, std::string_view> local_to_param;
        for (const auto& scan : ctx.nodes) {
            if (scan.type != NodeType::LocalDecl) continue;
            if (scan.as.local_decl.ident_count != 1 || scan.as.local_decl.value_count != 1) continue;
            uint32_t id_idx = ctx.block_statements[scan.as.local_decl.first_ident];
            uint32_t val_idx = ctx.block_statements[scan.as.local_decl.first_value];
            if (id_idx >= ctx.nodes.size() || val_idx >= ctx.nodes.size()) continue;
            if (ctx.nodes[id_idx].type != NodeType::Identifier) continue;
            if (ctx.nodes[val_idx].type != NodeType::TableAccess) continue;
            uint32_t tbl = ctx.nodes[val_idx].as.table_access.table;
            if (tbl >= ctx.nodes.size() || ctx.nodes[tbl].type != NodeType::Identifier) continue;
            std::string_view pname(ctx.nodes[tbl].as.ident.name, ctx.nodes[tbl].as.ident.length);
            if (!func_params.count(pname)) continue;
            if (state.numeric_table_fields.count(pname)) continue;
            std::string_view lname(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
            local_to_param[lname] = pname;
        }
        if (local_to_param.empty()) continue;
        std::map<std::string_view, std::set<std::string_view>> param_arith_fields;
        for (const auto& bn : ctx.nodes) {
            if (bn.type != NodeType::BinaryOp) continue;
            std::function<void(uint32_t)> check_side;
            check_side = [&](uint32_t side_idx) {
                if (side_idx >= ctx.nodes.size()) return;
                const auto& sn = ctx.nodes[side_idx];
                if (sn.type == NodeType::TableAccess) {
                    uint32_t stbl = sn.as.table_access.table;
                    if (stbl < ctx.nodes.size() && ctx.nodes[stbl].type == NodeType::Identifier) {
                        std::string_view sname(ctx.nodes[stbl].as.ident.name, ctx.nodes[stbl].as.ident.length);
                        auto it = local_to_param.find(sname);
                        if (it != local_to_param.end()) {
                            if (sn.as.table_access.key < ctx.nodes.size() && ctx.nodes[sn.as.table_access.key].type == NodeType::String) {
                                std::string_view fname(ctx.nodes[sn.as.table_access.key].as.string.text, ctx.nodes[sn.as.table_access.key].as.string.length);
                                param_arith_fields[it->second].insert(fname);
                            }
                        }
                    }
                } else if (sn.type == NodeType::ParenExpression) {
                    check_side(sn.as.paren_expr.expr);
                }
            };
            check_side(bn.as.bin_op.left);
            check_side(bn.as.bin_op.right);
        }
        for (auto& [pn, fields] : param_arith_fields) {
            if (!fields.empty() && !state.numeric_table_fields.count(pn)) {
                state.numeric_table_fields[pn] = fields;
                for (auto& fld : fields) {
                    if (std::find(state.string_pool.begin(), state.string_pool.end(), fld) == state.string_pool.end())
                        state.string_pool.push_back(fld);
                }
            }
        }
        for (auto& [ln, pn] : local_to_param) {
            auto it = param_arith_fields.find(pn);
            if (it != param_arith_fields.end() && !it->second.empty()) {
                if (!state.numeric_table_fields.count(ln)) {
                    state.numeric_table_fields[ln] = it->second;
                }
            }
        }
    }

    {

        int safety2 = 100;
        bool changed2;
        do {
            changed2 = false;
            if (--safety2 <= 0) break;
            for (const auto& node : ctx.nodes) {
                if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement) {
                    if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard) continue;
                    uint32_t ic = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count : node.as.global_decl.ident_count;
                    uint32_t fi = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident : node.as.global_decl.first_ident;
                    uint32_t fv = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value : node.as.global_decl.first_value;
                    uint32_t vc = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count : node.as.global_decl.value_count;
                    for (uint32_t ii = 0; ii < ic; ++ii) {
                        uint32_t idi = ctx.block_statements[fi + ii];
                        std::string_view nm(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                        uint32_t vi = (ii < vc) ? ctx.block_statements[fv + ii] : 0xFFFFFFFF;
                        if (disqualified.count(nm)) { if (known_numbers.erase(nm)) changed2 = true; }
                        else if (ctx.nodes[idi].as.ident.is_captured || ctx.nodes[idi].as.ident.is_global) { disqualified.insert(nm); if (known_numbers.erase(nm)) changed2 = true; }
                        else if (yields_number(ctx, state, vi, &known_numbers)) { if (known_numbers.insert(nm).second) changed2 = true; }
                        else { disqualified.insert(nm); if (known_numbers.erase(nm)) changed2 = true; }
                    }
                } else if (node.type == NodeType::Assignment) {
                    for (uint32_t ii = 0; ii < node.as.assign.target_count; ++ii) {
                        uint32_t ti = ctx.block_statements[node.as.assign.first_target + ii];
                        const auto& tn = ctx.nodes[ti];
                        if (tn.type == NodeType::Identifier) {
                            std::string_view nm(tn.as.ident.name, tn.as.ident.length);
                            uint32_t vi = (ii < node.as.assign.value_count) ? ctx.block_statements[node.as.assign.first_value + ii] : 0xFFFFFFFF;
                            if (disqualified.count(nm)) { if (known_numbers.erase(nm)) changed2 = true; }
                            else if (tn.as.ident.is_global) { disqualified.insert(nm); if (known_numbers.erase(nm)) changed2 = true; }
                            else if (yields_number(ctx, state, vi, &known_numbers)) { if (!disqualified.count(nm) && known_numbers.insert(nm).second) changed2 = true; }
                            else { disqualified.insert(nm); if (known_numbers.erase(nm)) changed2 = true; }
                        }
                    }
                } else if (node.type == NodeType::GenericForStatement) {
                    for (uint32_t ii = 0; ii < node.as.generic_for.var_count; ++ii) {
                        uint32_t vi = ctx.block_statements[node.as.generic_for.first_var + ii];
                        if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::Identifier) {
                            std::string_view nm(ctx.nodes[vi].as.ident.name, ctx.nodes[vi].as.ident.length);
                            disqualified.insert(nm);
                            if (known_numbers.erase(nm)) changed2 = true;
                        }
                    }
                }
            }
        } while (changed2);
    }

    state.param_numbers.clear();
    for (const auto& node : ctx.nodes) {
        if (node.type == NodeType::LocalDecl) {
            for (uint32_t ii = 0; ii < node.as.local_decl.ident_count; ++ii) {
                uint32_t idi = ctx.block_statements[node.as.local_decl.first_ident + ii];
                if (idi >= ctx.nodes.size() || ctx.nodes[idi].type != NodeType::Identifier) continue;
                std::string nm(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                if (known_numbers.count(nm) && !state.native_integers.count(nm)) {
                    uint32_t vi = (ii < node.as.local_decl.value_count)
                        ? ctx.block_statements[node.as.local_decl.first_value + ii] : 0xFFFFFFFF;
                    if (is_purely_integer_expr(ctx, state, vi))
                        state.native_integers.insert(std::string(nm));
                }
            }
        } else if (node.type == NodeType::Assignment) {
            for (uint32_t ii = 0; ii < node.as.assign.target_count; ++ii) {
                uint32_t ti = ctx.block_statements[node.as.assign.first_target + ii];
                if (ti >= ctx.nodes.size() || ctx.nodes[ti].type != NodeType::Identifier) continue;
                std::string_view nm(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length);
                if (known_numbers.count(nm) && !state.native_integers.count(std::string(nm))) {
                    uint32_t vi = (ii < node.as.assign.value_count)
                        ? ctx.block_statements[node.as.assign.first_value + ii] : 0xFFFFFFFF;
                    if (is_purely_integer_expr(ctx, state, vi))
                        state.native_integers.insert(std::string(nm));
                }
            }
        }
    }

    std::set<std::string_view> param_names;
    std::set<std::string> numeric_params;
    for (const auto& nd : ctx.nodes) {
        if (nd.type == NodeType::FunctionDef) {
            for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
                uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
                if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier) {
                    auto pn = std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length);
                    param_names.insert(pn);
                }
            }
        }
    }
    int _np_debug_node_count = 0;
    state.param_numbers.clear();
    std::map<std::string_view, std::map<uint32_t, uint32_t>> func_to_param_indices;
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto& nd = ctx.nodes[ni];
        if (nd.type != NodeType::FunctionDef) continue;
        _np_debug_node_count++;
        for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
            uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
            if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier)
                func_to_param_indices[std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length)][ni] = (uint32_t)p;
        }
    }
    std::map<uint32_t, std::vector<std::string_view>> func_def_params;
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto& nd = ctx.nodes[ni];
        if (nd.type != NodeType::FunctionDef) continue;
        _np_debug_node_count++;
        for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
            uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
            if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier)
                func_def_params[ni].push_back(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
        }
    }

    bool np_changed = true;
    for (int iter = 0; iter < 10 && np_changed; ++iter) {
        np_changed = false;
         for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
            const auto& nd = ctx.nodes[ni];
            if (nd.type != NodeType::FunctionDef) continue;
            auto& fparams = func_def_params[ni];
            if (fparams.empty()) continue;

            std::vector<uint32_t> stack = { nd.as.func_def.body_block };
            while (!stack.empty()) {
                uint32_t nid = stack.back(); stack.pop_back();
                if (nid == 0xFFFFFFFF || nid >= ctx.nodes.size()) continue;
                const auto& nn = ctx.nodes[nid];

                if (nn.type == NodeType::Block) {
                    for (uint32_t bi = 0; bi < nn.as.block.count; ++bi)
                        stack.push_back(ctx.block_statements[nn.as.block.first_statement + bi]);
                }
                if (nn.type == NodeType::LocalDecl) {
                    for (uint32_t vi = 0; vi < nn.as.local_decl.value_count; ++vi)
                        stack.push_back(ctx.block_statements[nn.as.local_decl.first_value + vi]);
                }
                if (nn.type == NodeType::Assignment) {
                    for (uint32_t vi = 0; vi < nn.as.assign.value_count; ++vi)
                        stack.push_back(ctx.block_statements[nn.as.assign.first_value + vi]);
                }
                if (nn.type == NodeType::ForStatement) {
                    stack.push_back(nn.as.for_stmt.start_expr);
                    stack.push_back(nn.as.for_stmt.limit_expr);
                    if (nn.as.for_stmt.step_expr != 0xFFFFFFFF) stack.push_back(nn.as.for_stmt.step_expr);
                    stack.push_back(nn.as.for_stmt.body_block);
                }
                if (nn.type == NodeType::IfStatement) {
                    stack.push_back(nn.as.if_stmt.condition);
                    stack.push_back(nn.as.if_stmt.then_block);
                    if (nn.as.if_stmt.else_block != 0xFFFFFFFF) stack.push_back(nn.as.if_stmt.else_block);
                }
                if (nn.type == NodeType::WhileStatement || nn.type == NodeType::RepeatStatement) {
                    stack.push_back(nn.as.while_stmt.condition);
                    stack.push_back(nn.as.while_stmt.body_block);
                }
                if (nn.type == NodeType::BinaryOp) {
                    stack.push_back(nn.as.bin_op.left);
                    stack.push_back(nn.as.bin_op.right);
                }
                if (nn.type == NodeType::UnaryOp) {
                    stack.push_back(nn.as.unary_op.expr);
                }
                if (nn.type == NodeType::ParenExpression) {
                    stack.push_back(nn.as.paren_expr.expr);
                }

                if (nn.type == NodeType::BinaryOp) {
                    int op = nn.as.bin_op.op;
                    if ((op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) ||
                        (op >= static_cast<int>(BinaryOp::Mod) && op <= static_cast<int>(BinaryOp::Shr))) {
                        auto check_arith = [&](uint32_t idx) {
                            if (idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::Identifier) {
                                std::string_view nm(ctx.nodes[idx].as.ident.name, ctx.nodes[idx].as.ident.length);
                                for (auto& fp : fparams) {
                                    if (fp == nm) {
                                        if (!state.param_numbers[ni].count(std::string(nm))) {
                                            state.param_numbers[ni].insert(std::string(nm));
                                            np_changed = true;
                                        }
                                    }
                                }
                            }
                        };
                        check_arith(nn.as.bin_op.left);
                        check_arith(nn.as.bin_op.right);
                    }
                }
                if (nn.type == NodeType::CallExpression) {
                    uint32_t tgt = nn.as.call_expr.target;
                    if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier) {
                        std::string_view callee(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                        auto cit = func_to_param_indices.find(callee);
                        if (cit != func_to_param_indices.end()) {
                            for (auto& [ci, start_idx] : cit->second) {
                                auto& cparams = func_def_params[ci];
                                for (size_t ai = 0; ai < nn.as.call_expr.arg_count && ai < cparams.size(); ++ai) {
                                    uint32_t arg_nid = ctx.block_statements[nn.as.call_expr.first_arg + ai];
                                    if (arg_nid < ctx.nodes.size() && ctx.nodes[arg_nid].type == NodeType::Identifier) {
                                        std::string_view arg_nm(ctx.nodes[arg_nid].as.ident.name, ctx.nodes[arg_nid].as.ident.length);
                                        std::string callee_pn(cparams[ai]);
                                        if (state.param_numbers[ci].count(callee_pn) && !state.param_numbers[ni].count(std::string(arg_nm))) {
                                            for (auto& fp : fparams) {
                                                if (fp == arg_nm) {
                                                    state.param_numbers[ni].insert(std::string(arg_nm));
                                                    np_changed = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                auto push_block = [&](uint32_t b) { if (b != 0xFFFFFFFF) stack.push_back(b); };
                if (nn.type == NodeType::Block)
                    for (uint32_t i = 0; i < nn.as.block.count; ++i)
                        push_block(ctx.block_statements[nn.as.block.first_statement + i]);
                else if (nn.type == NodeType::FunctionDef) {}
                else if (nn.type == NodeType::IfStatement) { push_block(nn.as.if_stmt.condition); push_block(nn.as.if_stmt.then_block); push_block(nn.as.if_stmt.else_block); }
                else if (nn.type == NodeType::WhileStatement) { push_block(nn.as.while_stmt.condition); push_block(nn.as.while_stmt.body_block); }
                else if (nn.type == NodeType::RepeatStatement) { push_block(nn.as.repeat_stmt.condition); push_block(nn.as.repeat_stmt.body_block); }
                else if (nn.type == NodeType::ForStatement) { push_block(nn.as.for_stmt.body_block); }
                else if (nn.type == NodeType::GenericForStatement) { push_block(nn.as.generic_for.body_block); }
                else if (nn.type == NodeType::ReturnStatement) {
                    for (uint32_t ri = 0; ri < nn.as.return_stmt.value_count; ++ri)
                        push_block(ctx.block_statements[nn.as.return_stmt.first_value + ri]);
                } else if (nn.type == NodeType::Assignment) {
                    for (uint32_t ai = 0; ai < nn.as.assign.value_count; ++ai)
                        push_block(ctx.block_statements[nn.as.assign.first_value + ai]);
                } else if (nn.type == NodeType::LocalDecl) {
                    for (uint32_t li = 0; li < nn.as.local_decl.value_count; ++li)
                        push_block(ctx.block_statements[nn.as.local_decl.first_value + li]);
                }
            }
        }
    }
    state.native_numbers.clear();
    for (auto name : known_numbers) {
        if (disqualified.find(name) == disqualified.end() && param_names.find(name) == param_names.end()) {
            state.native_numbers.push_back(name);
        }
    }

    // Build node→function owner map: for each FunctionDef, mark all nodes within its body
    state.node_func_owner.clear();
    for (uint32_t fi = 0; fi < ctx.nodes.size(); ++fi) {
        const auto& fn = ctx.nodes[fi];
        if (fn.type != NodeType::FunctionDef) continue;
        uint32_t body = fn.as.func_def.body_block;
        if (body >= ctx.nodes.size()) continue;
        std::vector<uint32_t> stk = {body};
        while (!stk.empty()) {
            uint32_t ni = stk.back(); stk.pop_back();
            if (ni >= ctx.nodes.size()) continue;
            state.node_func_owner[ni] = fi;
            const auto& nn = ctx.nodes[ni];
            auto push = [&](uint32_t idx) { if (idx < ctx.nodes.size()) stk.push_back(idx); };
            if (nn.type == NodeType::Block)
                for (uint32_t bi = 0; bi < nn.as.block.count; ++bi)
                    push(ctx.block_statements[nn.as.block.first_statement + bi]);
            if (nn.type == NodeType::ForStatement) { push(nn.as.for_stmt.start_expr); push(nn.as.for_stmt.limit_expr); if (nn.as.for_stmt.step_expr != 0xFFFFFFFF) push(nn.as.for_stmt.step_expr); push(nn.as.for_stmt.body_block); }
            if (nn.type == NodeType::WhileStatement || nn.type == NodeType::RepeatStatement) { push(nn.as.while_stmt.condition); push(nn.as.while_stmt.body_block); }
            if (nn.type == NodeType::IfStatement) { push(nn.as.if_stmt.condition); if (nn.as.if_stmt.then_block != 0xFFFFFFFF) push(nn.as.if_stmt.then_block); if (nn.as.if_stmt.else_block != 0xFFFFFFFF) push(nn.as.if_stmt.else_block); }
            if (nn.type == NodeType::BinaryOp) { push(nn.as.bin_op.left); push(nn.as.bin_op.right); }
            if (nn.type == NodeType::UnaryOp) { push(nn.as.unary_op.expr); }
            if (nn.type == NodeType::ParenExpression) { push(nn.as.paren_expr.expr); }
            if (nn.type == NodeType::TableAccess) { push(nn.as.table_access.table); push(nn.as.table_access.key); }
            if (nn.type == NodeType::CallExpression) { for (uint32_t ai = 0; ai < nn.as.call_expr.arg_count; ++ai) push(ctx.block_statements[nn.as.call_expr.first_arg + ai]); }
            if (nn.type == NodeType::LocalDecl) { for (uint32_t vi = 0; vi < nn.as.local_decl.value_count; ++vi) push(ctx.block_statements[nn.as.local_decl.first_value + vi]); }
            if (nn.type == NodeType::Assignment) { for (uint32_t vi = 0; vi < nn.as.assign.value_count; ++vi) push(ctx.block_statements[nn.as.assign.first_value + vi]); }
            if (nn.type == NodeType::ReturnStatement) { for (uint32_t vi = 0; vi < nn.as.return_stmt.value_count; ++vi) push(ctx.block_statements[nn.as.return_stmt.first_value + vi]); }
            if (nn.type == NodeType::FunctionDef) { push(nn.as.func_def.body_block); }
        }
    }

    // Pass 4: Detect function parameters used as pure numeric arrays (integer-keyed)
    // e.g., function MultiplyAv(N, v, out) where v[j] is used in arithmetic
    {
        // Collect all loop variable names (for j = 1, N do → j is always numeric)
        std::set<std::string_view> loop_vars;
        for (const auto& nd : ctx.nodes) {
            if (nd.type == NodeType::ForStatement) {
                uint32_t vi = nd.as.for_stmt.var_ident;
                if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::Identifier) {
                    loop_vars.insert(std::string_view(ctx.nodes[vi].as.ident.name, ctx.nodes[vi].as.ident.length));
                }
            }
        }

        // Collect all function definitions (both top-level and local)
        std::vector<const ASTNode*> func_defs;
        for (const auto& nd : ctx.nodes) {
            if (nd.type == NodeType::FunctionDef) {
                func_defs.push_back(&nd);
            } else if (nd.type == NodeType::LocalDecl) {
                for (uint32_t ii = 0; ii < nd.as.local_decl.value_count; ++ii) {
                    uint32_t vi = ctx.block_statements[nd.as.local_decl.first_value + ii];
                    if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::FunctionDef) {
                        func_defs.push_back(&ctx.nodes[vi]);
                    }
                }
            }
        }
        for (const auto* fnd : func_defs) {
            const auto& nd = *fnd;
            // Collect function parameter names
            std::set<std::string_view> func_params;
            for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
                uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
                if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier)
                    func_params.insert(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
            }
            if (func_params.empty()) continue;

            // Check if any parameter is reassigned — if so, skip it
            for (const auto& scan : ctx.nodes) {
                if (scan.type == NodeType::Assignment) {
                    for (uint32_t ii = 0; ii < scan.as.assign.target_count; ++ii) {
                        uint32_t ti = ctx.block_statements[scan.as.assign.first_target + ii];
                        if (ctx.nodes[ti].type == NodeType::Identifier) {
                            std::string_view nm(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length);
                            func_params.erase(nm);
                        }
                    }
                }
            }
            if (func_params.empty()) continue;

            // Find parameters accessed with integer keys in arithmetic
            for (const auto& bn : ctx.nodes) {
                if (bn.type != NodeType::BinaryOp) continue;
                int bop = bn.as.bin_op.op;
                bool is_arith = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Div));
                if (!is_arith) continue;

                std::function<void(uint32_t)> check_side;
                check_side = [&](uint32_t side_idx) {
                    if (side_idx >= ctx.nodes.size()) return;
                    const auto& sn = ctx.nodes[side_idx];
                    if (sn.type == NodeType::TableAccess) {
                        uint32_t stbl = sn.as.table_access.table;
                        if (stbl < ctx.nodes.size() && ctx.nodes[stbl].type == NodeType::Identifier) {
                            std::string_view sname(ctx.nodes[stbl].as.ident.name, ctx.nodes[stbl].as.ident.length);
                            if (func_params.count(sname)) {
                                uint32_t key_idx = sn.as.table_access.key;
                                if (key_idx < ctx.nodes.size()) {
                                    const auto& kn = ctx.nodes[key_idx];
                                    bool key_is_num = (kn.type == NodeType::Number || kn.type == NodeType::Integer);
                                    if (!key_is_num && kn.type == NodeType::Identifier) {
                                        std::string_view knm(kn.as.ident.name, kn.as.ident.length);
                                        key_is_num = state.native_integers.count(knm) > 0;
                                        if (!key_is_num) {
                                            key_is_num = std::find(state.native_numbers.begin(), state.native_numbers.end(), knm) != state.native_numbers.end();
                                        }
                                        if (!key_is_num) {
                                            key_is_num = loop_vars.count(knm) > 0;
                                        }
                                    }
                                    if (key_is_num) {
                                        state.pure_numeric_func_params[sname].insert(static_cast<uint32_t>(fnd - ctx.nodes.data()));
                                    }
                                }
                            }
                        }
                    } else if (sn.type == NodeType::ParenExpression) {
                        check_side(sn.as.paren_expr.expr);
                    } else if (sn.type == NodeType::BinaryOp) {
                        check_side(sn.as.bin_op.left);
                        check_side(sn.as.bin_op.right);
                    }
                };
                check_side(bn.as.bin_op.left);
                check_side(bn.as.bin_op.right);
            }
        }
    }

    //-------- Escape analysis: classify which local tables can be arena-allocated
    state.escaping_vars.clear();
    state.arena_safe_table_nodes.clear();
    state.arena_table_sizes.clear();

    // For each function, find local tables and check if they escape
    for (uint32_t fi = 0; fi < ctx.nodes.size(); ++fi) {
        const auto& fn = ctx.nodes[fi];
        if (fn.type != NodeType::FunctionDef) continue;
        uint32_t body = fn.as.func_def.body_block;
        if (body == 0xFFFFFFFF || body >= ctx.nodes.size()) continue;

        // Collect local table declarations in this function
        struct LocalTable { std::string_view name; uint32_t decl_node; uint32_t ctor_node; };
        std::vector<LocalTable> local_tables;

        // Walk function body to find local decls with table constructors
        std::function<void(uint32_t)> walk_body = [&](uint32_t block_idx) {
            if (block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size()) return;
            const auto& blk = ctx.nodes[block_idx];
            if (blk.type != NodeType::Block) return;
            for (uint32_t si = 0; si < blk.as.block.count; ++si) {
                uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + si];
                if (stmt >= ctx.nodes.size()) continue;
                const auto& sn = ctx.nodes[stmt];
                if (sn.type == NodeType::LocalDecl) {
                    for (uint32_t li = 0; li < sn.as.local_decl.ident_count; ++li) {
                        uint32_t id_idx = ctx.block_statements[sn.as.local_decl.first_ident + li];
                        if (id_idx >= ctx.nodes.size() || ctx.nodes[id_idx].type != NodeType::Identifier) continue;
                        // Only consider non-captured, non-global locals
                        if (ctx.nodes[id_idx].as.ident.is_captured || ctx.nodes[id_idx].as.ident.is_global) continue;
                        std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                        if (li < sn.as.local_decl.value_count) {
                            uint32_t val_idx = ctx.block_statements[sn.as.local_decl.first_value + li];
                            if (val_idx < ctx.nodes.size() && ctx.nodes[val_idx].type == NodeType::TableConstructor) {
                                local_tables.push_back({name, stmt, val_idx});
                            }
                        }
                    }
                } else if (sn.type == NodeType::Block) { walk_body(stmt); }
                  else if (sn.type == NodeType::IfStatement) { walk_body(sn.as.if_stmt.then_block); walk_body(sn.as.if_stmt.else_block); }
                  else if (sn.type == NodeType::WhileStatement) { walk_body(sn.as.while_stmt.body_block); }
                  else if (sn.type == NodeType::RepeatStatement) { walk_body(sn.as.repeat_stmt.body_block); }
                  else if (sn.type == NodeType::ForStatement) { walk_body(sn.as.for_stmt.body_block); }
                  else if (sn.type == NodeType::GenericForStatement) { walk_body(sn.as.generic_for.body_block); }
                  else if (sn.type == NodeType::FunctionDef) { /* don't recurse into nested functions */ }
                  else if (sn.type == NodeType::DoStatement) { walk_body(sn.as.do_stmt.body_block); }
            }
        };
        walk_body(body);

        if (local_tables.empty()) continue;

        // For each local table, walk the full function body to detect escapes
        for (auto& lt : local_tables) {
            bool escapes = false;

            // Escape conditions:
            // 1. is_captured on any use
            // 2. returned from function
            // 3. stored in global
            // 4. passed as function argument
            // 5. used as method target
            // 6. stored in another table that escapes
            // 7. reassigned to an escaping var

            std::function<void(uint32_t)> check_escape = [&](uint32_t block_idx) {
                if (escapes || block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size()) return;
                const auto& blk = ctx.nodes[block_idx];
                if (blk.type != NodeType::Block) return;
                for (uint32_t si = 0; si < blk.as.block.count; ++si) {
                    if (escapes) return;
                    uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + si];
                    if (stmt >= ctx.nodes.size()) continue;
                    const auto& sn = ctx.nodes[stmt];

                    // Check if this statement references our table variable in an escaping context
                    std::function<bool(uint32_t)> refs_var = [&](uint32_t n_idx) -> bool {
                        if (n_idx >= ctx.nodes.size()) return false;
                        const auto& n = ctx.nodes[n_idx];
                        if (n.type == NodeType::Identifier) {
                            return std::string_view(n.as.ident.name, n.as.ident.length) == lt.name;
                        }
                        if (n.type == NodeType::TableAccess) return refs_var(n.as.table_access.table);
                        return false;
                    };

                    // Return statement: if var is returned, it escapes
                    if (sn.type == NodeType::ReturnStatement) {
                        for (uint32_t ri = 0; ri < sn.as.return_stmt.value_count; ++ri) {
                            uint32_t v_idx = ctx.block_statements[sn.as.return_stmt.first_value + ri];
                            if (refs_var(v_idx)) { escapes = true; return; }
                        }
                    }

                    // Assignment to global
                    if (sn.type == NodeType::Assignment || sn.type == NodeType::GlobalDeclStatement) {
                        uint32_t t_count = (sn.type == NodeType::GlobalDeclStatement) ? sn.as.global_decl.ident_count : sn.as.assign.target_count;
                        uint32_t first_t = (sn.type == NodeType::GlobalDeclStatement) ? sn.as.global_decl.first_ident : sn.as.assign.first_target;
                        for (uint32_t ti = 0; ti < t_count; ++ti) {
                            uint32_t tgt = ctx.block_statements[first_t + ti];
                            if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier && ctx.nodes[tgt].as.ident.is_global) {
                                // Check if the corresponding value references our table
                                uint32_t v_count = (sn.type == NodeType::GlobalDeclStatement) ? sn.as.global_decl.value_count : sn.as.assign.value_count;
                                uint32_t first_v = (sn.type == NodeType::GlobalDeclStatement) ? sn.as.global_decl.first_value : sn.as.assign.first_value;
                                if (ti < v_count) {
                                    uint32_t v_idx = ctx.block_statements[first_v + ti];
                                    if (refs_var(v_idx)) { escapes = true; return; }
                                }
                            }
                        }
                    }

                    // Local assignment: if assigned to another local that later escapes, track transitively
                    if (sn.type == NodeType::LocalDecl) {
                        for (uint32_t li = 0; li < sn.as.local_decl.ident_count; ++li) {
                            uint32_t id_idx = ctx.block_statements[sn.as.local_decl.first_ident + li];
                            if (id_idx < ctx.nodes.size() && ctx.nodes[id_idx].type == NodeType::Identifier) {
                                std::string_view tname(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                                if (li < sn.as.local_decl.value_count) {
                                    uint32_t v_idx = ctx.block_statements[sn.as.local_decl.first_value + li];
                                    if (refs_var(v_idx) && state.escaping_vars.count(tname)) { escapes = true; return; }
                                }
                            }
                        }
                    }
                    if (sn.type == NodeType::Assignment) {
                        for (uint32_t ti = 0; ti < sn.as.assign.target_count; ++ti) {
                            uint32_t tgt = ctx.block_statements[sn.as.assign.first_target + ti];
                            if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                                std::string_view tname(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                                if (ti < sn.as.assign.value_count) {
                                    uint32_t v_idx = ctx.block_statements[sn.as.assign.first_value + ti];
                                    if (refs_var(v_idx) && state.escaping_vars.count(tname)) { escapes = true; return; }
                                }
                            }
                        }
                    }

                    // Function call: if var is passed as argument, it escapes
                    if (sn.type == NodeType::LocalDecl || sn.type == NodeType::Assignment) {
                        uint32_t v_count = (sn.type == NodeType::LocalDecl) ? sn.as.local_decl.value_count : sn.as.assign.value_count;
                        uint32_t first_v = (sn.type == NodeType::LocalDecl) ? sn.as.local_decl.first_value : sn.as.assign.first_value;
                        for (uint32_t vi = 0; vi < v_count; ++vi) {
                            uint32_t v_idx = ctx.block_statements[first_v + vi];
                            if (v_idx < ctx.nodes.size()) {
                                const auto& vn = ctx.nodes[v_idx];
                                if (vn.type == NodeType::CallExpression) {
                                    for (uint32_t ai = 0; ai < vn.as.call_expr.arg_count; ++ai) {
                                        uint32_t a_idx = ctx.block_statements[vn.as.call_expr.first_arg + ai];
                                        if (refs_var(a_idx)) { escapes = true; return; }
                                    }
                                }
                            }
                        }
                    }

                    // Direct call expression statement
                    if (sn.type == NodeType::Block) { check_escape(stmt); continue; }
                    if (sn.type == NodeType::IfStatement) { check_escape(sn.as.if_stmt.then_block); check_escape(sn.as.if_stmt.else_block); continue; }
                    if (sn.type == NodeType::WhileStatement) { check_escape(sn.as.while_stmt.body_block); continue; }
                    if (sn.type == NodeType::RepeatStatement) { check_escape(sn.as.repeat_stmt.body_block); continue; }
                    if (sn.type == NodeType::ForStatement) { check_escape(sn.as.for_stmt.body_block); continue; }
                    if (sn.type == NodeType::GenericForStatement) { check_escape(sn.as.generic_for.body_block); continue; }
                    if (sn.type == NodeType::DoStatement) { check_escape(sn.as.do_stmt.body_block); continue; }
                    if (sn.type == NodeType::FunctionDef) { continue; } // don't recurse into nested funcs
                }
            };
            check_escape(body);

            // Also check: is_captured flag on the declaration itself
            if (!escapes) {
                for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
                    const auto& n = ctx.nodes[ni];
                    if (n.type == NodeType::Identifier && n.as.ident.is_captured &&
                        std::string_view(n.as.ident.name, n.as.ident.length) == lt.name) {
                        escapes = true;
                        break;
                    }
                }
            }

            if (!escapes) {
                // Also check if the table might grow (assigned via t[i]=v or t.key=v after creation)
                bool might_grow = false;
                std::function<void(uint32_t)> check_growth = [&](uint32_t block_idx) {
                    if (might_grow || block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size()) return;
                    const auto& blk = ctx.nodes[block_idx];
                    if (blk.type != NodeType::Block) return;
                    for (uint32_t si = 0; si < blk.as.block.count; ++si) {
                        if (might_grow) return;
                        uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + si];
                        if (stmt >= ctx.nodes.size()) continue;
                        const auto& sn = ctx.nodes[stmt];
                        // Check assignments where our variable is the table target: t[i] = v or t.key = v
                        auto check_target = [&](uint32_t tgt_idx) {
                            if (tgt_idx >= ctx.nodes.size()) return;
                            const auto& tn = ctx.nodes[tgt_idx];
                            if (tn.type == NodeType::TableAccess) {
                                if (tn.as.table_access.table < ctx.nodes.size() &&
                                    ctx.nodes[tn.as.table_access.table].type == NodeType::Identifier) {
                                    std::string_view tname(ctx.nodes[tn.as.table_access.table].as.ident.name,
                                                           ctx.nodes[tn.as.table_access.table].as.ident.length);
                                    if (tname == lt.name) might_grow = true;
                                }
                            }
                        };
                        if (sn.type == NodeType::Assignment) {
                            for (uint32_t ti = 0; ti < sn.as.assign.target_count; ++ti)
                                check_target(ctx.block_statements[sn.as.assign.first_target + ti]);
                        }
                        // Recurse into blocks
                        if (sn.type == NodeType::Block) check_growth(stmt);
                        else if (sn.type == NodeType::IfStatement) { check_growth(sn.as.if_stmt.then_block); check_growth(sn.as.if_stmt.else_block); }
                        else if (sn.type == NodeType::WhileStatement) check_growth(sn.as.while_stmt.body_block);
                        else if (sn.type == NodeType::RepeatStatement) check_growth(sn.as.repeat_stmt.body_block);
                        else if (sn.type == NodeType::ForStatement) check_growth(sn.as.for_stmt.body_block);
                        else if (sn.type == NodeType::GenericForStatement) check_growth(sn.as.generic_for.body_block);
                        else if (sn.type == NodeType::DoStatement) check_growth(sn.as.do_stmt.body_block);
                    }
                };
                check_growth(body);

                if (!might_grow) {
                    state.arena_safe_table_nodes.insert(lt.ctor_node);
                    state.escaping_vars.erase(lt.name);
                } else {
                    state.escaping_vars.insert(lt.name);
                }
            } else {
                state.escaping_vars.insert(lt.name);
            }
        }

        // Compute arena size for this function
        uint32_t total_arena = 0;
        for (uint32_t node_idx : state.arena_safe_table_nodes) {
            const auto& tc = ctx.nodes[node_idx];
            if (tc.type != NodeType::TableConstructor) continue;
            // Estimate array size from element count, accounting for table_presize
            size_t arr_count = 0;
            size_t hash_count = 0;
            for (uint32_t ei = 0; ei < tc.as.table_cons.count; ++ei) {
                uint32_t k_idx = ctx.block_statements[tc.as.table_cons.first_item + ei * 2];
                if (k_idx == 0xFFFFFFFF) { arr_count++; }
                else { hash_count++; }
            }
            // If table_presize exists, use it as the array size (it's the loop limit)
            if (state.table_presize.count(node_idx)) {
                arr_count = 16; // conservatively estimate loop-bounded tables
            }
            // Use hardcoded sizes matching runtime types (optimizer doesn't include clx_runtime.h)
            constexpr size_t HDR = 112;     // sizeof(LTable) padded
            constexpr size_t TVAL = 8;      // sizeof(TValue)
            constexpr size_t VT = 1;        // sizeof(ValueType)
            constexpr size_t HENTRY = 24;   // sizeof(HashEntry)
            // Apply the same minimum preallocation as arena_create_table
            // Must match CLX_ARENA_DEFAULT_FIELDS in clx_runtime.h
            constexpr size_t MIN_FIELDS = 8;
            if (arr_count < MIN_FIELDS) arr_count = MIN_FIELDS;
            if (hash_count < MIN_FIELDS) hash_count = MIN_FIELDS;
            size_t aligned_arr = ((TVAL * arr_count + 7) & ~static_cast<size_t>(7));
            size_t aligned_types = ((VT * arr_count + 7) & ~static_cast<size_t>(7));
            size_t aligned_hash = ((HENTRY * hash_count + 7) & ~static_cast<size_t>(7));
            total_arena += static_cast<uint32_t>(HDR + aligned_arr + aligned_types + aligned_hash);
        }
        if (total_arena > 0) {
            state.arena_table_sizes[fi] = total_arena;
        }
    }
}

}