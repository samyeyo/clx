// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  functions.cpp · Function callability passes│
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

//------------------ pass_callables: function index, direct/fast-callable qualification, native-direct funcs
void pass_callables(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &known_numbers = sk.known_numbers;
    auto &disqualified = sk.disqualified;
    auto &array_bounds = sk.array_bounds;
    auto &loop_limits = sk.loop_limits;
    auto &loop_limit_conflicts = sk.loop_limit_conflicts;
    auto &func_defs = sk.func_defs;
    auto &call_to_func = sk.call_to_func;
    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement) {
            uint32_t i_count
                = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count : node.as.global_decl.ident_count;
            uint32_t f_ident
                = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident : node.as.global_decl.first_ident;
            uint32_t f_value
                = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value : node.as.global_decl.first_value;
            uint32_t v_count
                = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count : node.as.global_decl.value_count;

            if (i_count == 1 && v_count == 1) {
                uint32_t t_idx = ctx.block_statements[f_ident];
                uint32_t v_idx = ctx.block_statements[f_value];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[v_idx].type == NodeType::FunctionDef) {
                    std::string_view fname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    if (state.reassigned_vars.count(fname) == 0) {
                        state.func_param_counts[fname] = ctx.nodes[v_idx].as.func_def.param_count;
                        state.func_param_native[fname]
                            = std::vector<bool>(ctx.nodes[v_idx].as.func_def.param_count, true);
                        func_defs.push_back({ fname, v_idx });
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
                        state.func_param_native[fname]
                            = std::vector<bool>(ctx.nodes[v_idx].as.func_def.param_count, true);
                        func_defs.push_back({ fname, v_idx });
                    }
                }
            }
        }
    }

    for (const auto &fdef : func_defs) {
        auto cb = [&](uint32_t idx) {
            if (ctx.nodes[idx].type == NodeType::CallExpression)
                call_to_func[idx] = fdef.first;
        };
        traverse_node(ctx, ctx.nodes[fdef.second].as.func_def.body_block, cb);
    }

    std::set<std::string_view> escaped_funcs;
    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto &n = ctx.nodes[i];

        auto check_escape = [&](uint32_t idx) {
            while (idx != 0xFFFFFFFF && idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::ParenExpression) {
                idx = ctx.nodes[idx].as.paren_expr.expr;
            }
            if (idx != 0xFFFFFFFF && idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::Identifier) {
                escaped_funcs.insert(std::string_view(ctx.nodes[idx].as.ident.name, ctx.nodes[idx].as.ident.length));
            }
        };

        if (n.type == NodeType::CallExpression) {
            for (uint32_t a = 0; a < n.as.call_expr.arg_count; ++a)
                check_escape(ctx.block_statements[n.as.call_expr.first_arg + a]);
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t a = 0; a < n.as.return_stmt.value_count; ++a)
                check_escape(ctx.block_statements[n.as.return_stmt.first_value + a]);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t a = 0; a < n.as.table_cons.count; ++a)
                check_escape(ctx.block_statements[n.as.table_cons.first_item + a * 2 + 1]);
        } else if (n.type == NodeType::Assignment) {
            for (uint32_t a = 0; a < n.as.assign.value_count; ++a)
                check_escape(ctx.block_statements[n.as.assign.first_value + a]);
        } else if (n.type == NodeType::LocalDecl) {
            for (uint32_t a = 0; a < n.as.local_decl.value_count; ++a)
                check_escape(ctx.block_statements[n.as.local_decl.first_value + a]);
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
        for (const auto &fdef : func_defs) {
            if (fdef.first == fname) {
                uint32_t f_idx = fdef.second;
                const auto &fn = ctx.nodes[f_idx];
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
        if (--safety_limit <= 0)
            break;

        for (const auto &node : ctx.nodes) {
            if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement) {
                if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard)
                    continue;
                uint32_t i_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count
                                                                      : node.as.global_decl.ident_count;
                uint32_t f_ident = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident
                                                                      : node.as.global_decl.first_ident;
                uint32_t f_value = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value
                                                                      : node.as.global_decl.first_value;
                uint32_t v_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count
                                                                      : node.as.global_decl.value_count;

                for (uint32_t i = 0; i < i_count; ++i) {
                    uint32_t id_idx = ctx.block_statements[f_ident + i];
                    std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                    uint32_t val_idx = (i < v_count) ? ctx.block_statements[f_value + i] : 0xFFFFFFFF;

                    if (disqualified.count(name)) {
                        if (known_numbers.erase(name))
                            changed = true;
                    } else if (ctx.nodes[id_idx].as.ident.is_captured || ctx.nodes[id_idx].as.ident.is_global) {
                        disqualified.insert(name);
                        if (known_numbers.erase(name))
                            changed = true;
                    } else if (yields_number(ctx, state, val_idx, &known_numbers)) {
                        if (known_numbers.insert(name).second)
                            changed = true;
                    } else {
                        disqualified.insert(name);
                        if (known_numbers.erase(name))
                            changed = true;
                    }
                }
            } else if (node.type == NodeType::Assignment) {
                for (uint32_t i = 0; i < node.as.assign.target_count; ++i) {
                    uint32_t t_idx = ctx.block_statements[node.as.assign.first_target + i];
                    const auto &t_node = ctx.nodes[t_idx];
                    if (t_node.type == NodeType::Identifier) {
                        std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
                        uint32_t val_idx = (i < node.as.assign.value_count)
                            ? ctx.block_statements[node.as.assign.first_value + i]
                            : 0xFFFFFFFF;

                        if (disqualified.count(name)) {
                            if (known_numbers.erase(name))
                                changed = true;
                        } else if (t_node.as.ident.is_global) {
                            disqualified.insert(name);
                            if (known_numbers.erase(name))
                                changed = true;
                        } else if (yields_number(ctx, state, val_idx, &known_numbers)) {
                            if (!disqualified.count(name) && known_numbers.insert(name).second)
                                changed = true;
                        } else {
                            disqualified.insert(name);
                            if (known_numbers.erase(name))
                                changed = true;
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

    for (auto &[fname, is_native_vec] : state.func_param_native) {
        for (const auto &fdef : func_defs) {
            if (fdef.first == fname) {
                uint32_t f_idx = fdef.second;
                const auto &fn = ctx.nodes[f_idx];
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

    for (auto &[fname, is_native_vec] : state.func_param_native) {
        for (const auto &fdef : func_defs) {
            if (fdef.first != fname)
                continue;
            const auto &fn = ctx.nodes[fdef.second];
            std::set<std::string_view> pnames;
            for (size_t p = 0; p < fn.as.func_def.param_count && p < is_native_vec.size(); ++p) {
                uint32_t pi = ctx.block_statements[fn.as.func_def.first_param + p];
                if (ctx.nodes[pi].type == NodeType::Identifier)
                    pnames.insert(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
            }
            if (pnames.empty())
                continue;
            auto nil_scan = [&](auto &self, uint32_t nidx) -> void {
                if (nidx >= ctx.nodes.size())
                    return;
                const auto &n = ctx.nodes[nidx];
                if (n.type == NodeType::BinaryOp && n.as.bin_op.op == static_cast<int>(BinaryOp::Eq)) {
                    auto try_disqualify = [&](uint32_t a, uint32_t b) {
                        if (ctx.nodes[a].type != NodeType::Identifier || ctx.nodes[b].type != NodeType::NilLiteral)
                            return;
                        std::string_view nm(ctx.nodes[a].as.ident.name, ctx.nodes[a].as.ident.length);
                        if (!pnames.count(nm))
                            return;
                        for (size_t p = 0; p < fn.as.func_def.param_count; ++p) {
                            uint32_t pi = ctx.block_statements[fn.as.func_def.first_param + p];
                            if (ctx.nodes[pi].type == NodeType::Identifier
                                && std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length) == nm) {
                                is_native_vec[p] = false;
                                break;
                            }
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
            const auto &node = ctx.nodes[n_idx];
            if (node.type == NodeType::CallExpression) {
                uint32_t tgt = node.as.call_expr.target;
                if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                    std::string_view fname(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                    if (state.func_param_native.count(fname)) {
                        std::string_view current_func = call_to_func.count(n_idx) ? call_to_func[n_idx] : "";
                        std::set<std::string_view> current_params;
                        if (!current_func.empty() && state.func_param_native.count(current_func)) {
                            uint32_t c_fdef_idx = 0xFFFFFFFF;
                            for (auto fd : func_defs)
                                if (fd.first == current_func)
                                    c_fdef_idx = fd.second;
                            if (c_fdef_idx != 0xFFFFFFFF) {
                                const auto &fn = ctx.nodes[c_fdef_idx];
                                for (size_t p = 0; p < fn.as.func_def.param_count; ++p) {
                                    if (state.func_param_native[current_func][p]) {
                                        uint32_t p_idx = ctx.block_statements[fn.as.func_def.first_param + p];
                                        current_params.insert(std::string_view(
                                            ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length));
                                    }
                                }
                            }
                        }

                        for (size_t p = 0; p < state.func_param_counts[fname]; ++p) {
                            if (state.func_param_native[fname][p]) {
                                if (p < node.as.call_expr.arg_count) {
                                    uint32_t arg_idx = ctx.block_statements[node.as.call_expr.first_arg + p];
                                    if (!yields_number(
                                            ctx, state, arg_idx, &known_numbers, current_func, &current_params)) {
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

    for (const auto &fdef : func_defs) {
        if (function_returns_native(ctx, state, fdef.second, fdef.first, &known_numbers)) {
            state.native_return_funcs.insert(fdef.first);
        }
    }

    //------------------ native-direct qualification: LOCAL, non-reassigned,

    for (const auto &ldnode : ctx.nodes) {
        if (ldnode.type != NodeType::LocalDecl)
            continue;
        if (ldnode.as.local_decl.ident_count != 1 || ldnode.as.local_decl.value_count != 1)
            continue;
        uint32_t t_idx = ctx.block_statements[ldnode.as.local_decl.first_ident];
        uint32_t v_idx = ctx.block_statements[ldnode.as.local_decl.first_value];
        if (ctx.nodes[t_idx].type != NodeType::Identifier || ctx.nodes[t_idx].as.ident.is_global)
            continue;
        if (ctx.nodes[v_idx].type != NodeType::FunctionDef)
            continue;
        std::string_view fname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
        if (state.reassigned_vars.count(fname))
            continue;
        {
            const auto &fn = ctx.nodes[v_idx];
            if (!fn.as.func_def.is_vararg) {
                bool nd_ok = true;
                bool nd_single = true;
                bool nd_callret = false;
                auto nd_scan = [&](auto &self, uint32_t nidx) -> void {
                    if (!nd_ok || nidx == 0xFFFFFFFF || nidx >= ctx.nodes.size())
                        return;
                    const auto &n = ctx.nodes[nidx];
                    if (n.type == NodeType::FunctionDef) {

                        nd_ok = false;
                        return;
                    }
                    if (n.type == NodeType::Identifier) {
                        if (n.as.ident.is_global)
                            nd_ok = false;
                        return;
                    }
                    if (n.type == NodeType::Vararg) {
                        nd_ok = false;
                        return;
                    }
                    if (n.type == NodeType::ReturnStatement) {
                        if (n.as.return_stmt.value_count != 1)
                            nd_single = false;
                        if (n.as.return_stmt.value_count > 1)
                            nd_ok = false;
                        if (n.as.return_stmt.value_count == 1) {
                            uint32_t rv = ctx.block_statements[n.as.return_stmt.first_value];
                            while (rv < ctx.nodes.size() && ctx.nodes[rv].type == NodeType::ParenExpression)
                                rv = ctx.nodes[rv].as.paren_expr.expr;
                            if (rv < ctx.nodes.size() && ctx.nodes[rv].type == NodeType::CallExpression)
                                nd_callret = true;
                        }
                    }
                    if (!nd_ok)
                        return;
                    if (n.type == NodeType::Block) {
                        for (uint32_t i = 0; i < n.as.block.count; ++i)
                            self(self, ctx.block_statements[n.as.block.first_statement + i]);
                    } else if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement
                        || n.type == NodeType::Assignment) {
                        uint32_t vc = (n.type == NodeType::LocalDecl)
                            ? n.as.local_decl.value_count
                            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count
                                                                         : n.as.assign.value_count);
                        uint32_t fv = (n.type == NodeType::LocalDecl)
                            ? n.as.local_decl.first_value
                            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value
                                                                         : n.as.assign.first_value);
                        for (uint32_t i = 0; i < vc; ++i)
                            self(self, ctx.block_statements[fv + i]);

                        uint32_t tc = (n.type == NodeType::LocalDecl)
                            ? n.as.local_decl.ident_count
                            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.ident_count
                                                                         : n.as.assign.target_count);
                        uint32_t ft = (n.type == NodeType::LocalDecl)
                            ? n.as.local_decl.first_ident
                            : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_ident
                                                                         : n.as.assign.first_target);
                        for (uint32_t i = 0; i < tc; ++i)
                            self(self, ctx.block_statements[ft + i]);
                    } else if (n.type == NodeType::BinaryOp) {
                        self(self, n.as.bin_op.left);
                        self(self, n.as.bin_op.right);
                    } else if (n.type == NodeType::UnaryOp) {
                        self(self, n.as.unary_op.expr);
                    } else if (n.type == NodeType::CallExpression) {
                        self(self, n.as.call_expr.target);
                        for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i)
                            self(self, ctx.block_statements[n.as.call_expr.first_arg + i]);
                    } else if (n.type == NodeType::IfStatement) {
                        self(self, n.as.if_stmt.condition);
                        self(self, n.as.if_stmt.then_block);
                        self(self, n.as.if_stmt.else_block);
                    } else if (n.type == NodeType::WhileStatement) {
                        self(self, n.as.while_stmt.condition);
                        self(self, n.as.while_stmt.body_block);
                    } else if (n.type == NodeType::RepeatStatement) {
                        self(self, n.as.repeat_stmt.body_block);
                        self(self, n.as.repeat_stmt.condition);
                    } else if (n.type == NodeType::ForStatement) {
                        self(self, n.as.for_stmt.start_expr);
                        self(self, n.as.for_stmt.limit_expr);
                        self(self, n.as.for_stmt.step_expr);
                        self(self, n.as.for_stmt.body_block);
                    } else if (n.type == NodeType::GenericForStatement) {
                        for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                            self(self, ctx.block_statements[n.as.generic_for.first_iter + i]);
                        self(self, n.as.generic_for.body_block);
                    } else if (n.type == NodeType::DoStatement) {
                        self(self, n.as.do_stmt.body_block);
                    } else if (n.type == NodeType::TableConstructor) {
                        for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                            self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2]);
                            self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1]);
                        }
                    } else if (n.type == NodeType::TableAccess) {
                        self(self, n.as.table_access.table);
                        self(self, n.as.table_access.key);
                    } else if (n.type == NodeType::ReturnStatement) {
                        for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
                            self(self, ctx.block_statements[n.as.return_stmt.first_value + i]);
                    } else if (n.type == NodeType::ParenExpression) {
                        self(self, n.as.paren_expr.expr);
                    }
                };
                nd_scan(nd_scan, fn.as.func_def.body_block);

                bool nd_exact = nd_ok && nd_single && !nd_callret;
                if (nd_exact) {

                    auto apr = [&](auto &self, uint32_t bidx) -> bool {
                        if (bidx == 0xFFFFFFFF || bidx >= ctx.nodes.size())
                            return false;
                        if (ctx.nodes[bidx].type != NodeType::Block)
                            return false;
                        const auto &blk = ctx.nodes[bidx];
                        for (uint32_t i = 0; i < blk.as.block.count; ++i) {
                            uint32_t si = ctx.block_statements[blk.as.block.first_statement + i];
                            if (si >= ctx.nodes.size())
                                continue;
                            const auto &st = ctx.nodes[si];
                            if (st.type == NodeType::ReturnStatement)
                                return true;
                            if (st.type == NodeType::IfStatement) {
                                bool then_r = self(self, st.as.if_stmt.then_block);
                                uint32_t eb = st.as.if_stmt.else_block;

                                bool else_r = false;
                                if (eb != 0xFFFFFFFF && eb < ctx.nodes.size()) {
                                    if (ctx.nodes[eb].type == NodeType::IfStatement) {
                                        uint32_t db = eb;
                                        else_r = true;
                                        while (db != 0xFFFFFFFF && db < ctx.nodes.size()
                                            && ctx.nodes[db].type == NodeType::IfStatement) {
                                            if (!self(self, ctx.nodes[db].as.if_stmt.then_block)) {
                                                else_r = false;
                                                break;
                                            }
                                            db = ctx.nodes[db].as.if_stmt.else_block;
                                        }
                                        if (else_r)
                                            else_r = self(self, db);
                                    } else {
                                        else_r = self(self, eb);
                                    }
                                }
                                if (then_r && else_r)
                                    return true;
                            } else if (st.type == NodeType::DoStatement) {
                                if (self(self, st.as.do_stmt.body_block))
                                    return true;
                            }
                        }
                        return false;
                    };
                    if (!apr(apr, fn.as.func_def.body_block))
                        nd_exact = false;
                }
                if (nd_exact) {

                    uint32_t nparams = fn.as.func_def.param_count;
                    auto sscan = [&](auto &self, uint32_t nidx) -> void {
                        if (!nd_exact || nidx == 0xFFFFFFFF || nidx >= ctx.nodes.size())
                            return;
                        const auto &n = ctx.nodes[nidx];
                        if (n.type == NodeType::CallExpression) {
                            uint32_t tg = n.as.call_expr.target;
                            if (tg < ctx.nodes.size() && ctx.nodes[tg].type == NodeType::Identifier
                                && !ctx.nodes[tg].as.ident.is_global
                                && std::string_view(ctx.nodes[tg].as.ident.name, ctx.nodes[tg].as.ident.length) == fname
                                && (n.as.call_expr.arg_count != nparams)) {
                                nd_exact = false;
                                return;
                            }
                            if (tg < ctx.nodes.size() && ctx.nodes[tg].type == NodeType::Identifier
                                && !ctx.nodes[tg].as.ident.is_global
                                && std::string_view(ctx.nodes[tg].as.ident.name, ctx.nodes[tg].as.ident.length) == fname
                                && n.as.call_expr.arg_count > 0) {
                                uint32_t la
                                    = ctx.block_statements[n.as.call_expr.first_arg + n.as.call_expr.arg_count - 1];
                                if (la < ctx.nodes.size()
                                    && (ctx.nodes[la].type == NodeType::CallExpression
                                        || ctx.nodes[la].type == NodeType::Vararg))
                                    nd_exact = false;
                            }
                        }
                        if (!nd_exact)
                            return;
                        if (n.type == NodeType::Block) {
                            for (uint32_t i = 0; i < n.as.block.count; ++i)
                                self(self, ctx.block_statements[n.as.block.first_statement + i]);
                        } else if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement
                            || n.type == NodeType::Assignment) {
                            uint32_t vc = (n.type == NodeType::LocalDecl)
                                ? n.as.local_decl.value_count
                                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count
                                                                             : n.as.assign.value_count);
                            uint32_t fv = (n.type == NodeType::LocalDecl)
                                ? n.as.local_decl.first_value
                                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value
                                                                             : n.as.assign.first_value);
                            for (uint32_t i = 0; i < vc; ++i)
                                self(self, ctx.block_statements[fv + i]);
                        } else if (n.type == NodeType::BinaryOp) {
                            self(self, n.as.bin_op.left);
                            self(self, n.as.bin_op.right);
                        } else if (n.type == NodeType::UnaryOp) {
                            self(self, n.as.unary_op.expr);
                        } else if (n.type == NodeType::CallExpression) {
                            self(self, n.as.call_expr.target);
                            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i)
                                self(self, ctx.block_statements[n.as.call_expr.first_arg + i]);
                        } else if (n.type == NodeType::IfStatement) {
                            self(self, n.as.if_stmt.condition);
                            self(self, n.as.if_stmt.then_block);
                            self(self, n.as.if_stmt.else_block);
                        } else if (n.type == NodeType::WhileStatement) {
                            self(self, n.as.while_stmt.condition);
                            self(self, n.as.while_stmt.body_block);
                        } else if (n.type == NodeType::RepeatStatement) {
                            self(self, n.as.repeat_stmt.body_block);
                            self(self, n.as.repeat_stmt.condition);
                        } else if (n.type == NodeType::ForStatement) {
                            self(self, n.as.for_stmt.start_expr);
                            self(self, n.as.for_stmt.limit_expr);
                            self(self, n.as.for_stmt.step_expr);
                            self(self, n.as.for_stmt.body_block);
                        } else if (n.type == NodeType::GenericForStatement) {
                            for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                                self(self, ctx.block_statements[n.as.generic_for.first_iter + i]);
                            self(self, n.as.generic_for.body_block);
                        } else if (n.type == NodeType::DoStatement) {
                            self(self, n.as.do_stmt.body_block);
                        } else if (n.type == NodeType::TableConstructor) {
                            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2]);
                                self(self, ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1]);
                            }
                        } else if (n.type == NodeType::TableAccess) {
                            self(self, n.as.table_access.table);
                            self(self, n.as.table_access.key);
                        } else if (n.type == NodeType::ReturnStatement) {
                            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
                                self(self, ctx.block_statements[n.as.return_stmt.first_value + i]);
                        } else if (n.type == NodeType::FunctionDef) {
                            self(self, n.as.func_def.body_block);
                        } else if (n.type == NodeType::ParenExpression) {
                            self(self, n.as.paren_expr.expr);
                        }
                    };
                    sscan(sscan, fn.as.func_def.body_block);
                }
                if (nd_ok && nd_exact)
                    state.native_direct_funcs.insert(fname);
            }
        }
    }

    for (const auto &fdef : func_defs) {
        if (state.func_param_native.count(fdef.first)) {
            const auto &fn = ctx.nodes[fdef.second].as.func_def;
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

            if (!t_str.empty() && !k_str.empty() && array_bounds.count(t_str) && loop_limits.count(k_str)
                && !loop_limit_conflicts[k_str]) {
                uint32_t bound_expr = array_bounds[t_str];
                uint32_t loop_expr = loop_limits[k_str];

                if (bound_expr == loop_expr) {
                    state.bce_safe_nodes.insert(i);
                } else {
                    std::string b_str = get_ast_string(ctx, bound_expr);
                    std::string l_str = get_ast_string(ctx, loop_expr);
                    if (!b_str.empty() && !l_str.empty()) {
                        if (b_str == l_str)
                            state.bce_safe_nodes.insert(i);
                        else if (l_str.find(b_str + " -") == 0)
                            state.bce_safe_nodes.insert(i);
                    } else {
                        double bd = 0, ld = 0;
                        int64_t bi = 0, li = 0;
                        bool bint, lint;
                        if (is_literal_number(ctx, bound_expr, bd, bi, bint)
                            && is_literal_number(ctx, loop_expr, ld, li, lint)) {
                            if (bd >= ld)
                                state.bce_safe_nodes.insert(i);
                        }
                    }
                }
            }
        }
    }

    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::String) {
            std::string_view s(node.as.string.text, node.as.string.length);
            if (state.string_pool_index.find(s) == state.string_pool_index.end()) {
                state.string_pool_index[s] = state.string_pool.size();
                state.string_pool.push_back(s);
            }
        }
        if (node.type == NodeType::Identifier) {
            std::string_view s(node.as.ident.name, node.as.ident.length);
            if (state.string_pool_index.find(s) == state.string_pool_index.end()) {
                state.string_pool_index[s] = state.string_pool.size();
                state.string_pool.push_back(s);
            }
        }
    }
}

}
