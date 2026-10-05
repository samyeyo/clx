// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  tables.cpp · Table analysis passes         │
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

//------------------ pass_pure_arrays: pure numeric array detection and disqualification
void pass_pure_arrays(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &known_numbers = sk.known_numbers;
    auto &disqualified_arrays = sk.disqualified_arrays;
    auto &empty_array_candidates = sk.empty_array_candidates;
    auto &array_bounds = sk.array_bounds;
    state.pure_numeric_arrays.clear();
    std::set<std::string_view> pure_candidates;

    //------------------ bare_ident: unwraps ParenExpression layers and returns the identifier name

    auto bare_ident = [&](uint32_t nidx) -> std::string_view {
        while (nidx < ctx.nodes.size() && ctx.nodes[nidx].type == NodeType::ParenExpression)
            nidx = ctx.nodes[nidx].as.paren_expr.expr;
        if (nidx < ctx.nodes.size() && ctx.nodes[nidx].type == NodeType::Identifier)
            return std::string_view(ctx.nodes[nidx].as.ident.name, ctx.nodes[nidx].as.ident.length);
        return { };
    };

    //------------------ disq_bare: disqualify the array used bare (or parenthesized) in a value position codegen cannot lower.

    auto disq_bare = [&](uint32_t nidx) {
        std::string_view nm = bare_ident(nidx);
        if (!nm.empty())
            disqualified_arrays.insert(nm);
    };

    //------------------ vector_iter_calls: generic-for ipairs(p)/pairs(p) exempt from the call-argument rule (native-loop lowering).

    std::set<uint32_t> vector_iter_calls;
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto &vn = ctx.nodes[ni];
        if (vn.type != NodeType::GenericForStatement || vn.as.generic_for.iter_count != 1)
            continue;
        uint32_t it = ctx.block_statements[vn.as.generic_for.first_iter];
        if (it >= ctx.nodes.size() || ctx.nodes[it].type != NodeType::CallExpression)
            continue;
        const auto &cn = ctx.nodes[it];
        if (cn.as.call_expr.arg_count != 1)
            continue;
        uint32_t tg = cn.as.call_expr.target;
        if (tg >= ctx.nodes.size() || ctx.nodes[tg].type != NodeType::Identifier || !ctx.nodes[tg].as.ident.is_global)
            continue;
        std::string_view fn(ctx.nodes[tg].as.ident.name, ctx.nodes[tg].as.ident.length);
        if ((fn == "ipairs" || fn == "pairs") && state.reassigned_vars.count(fn) == 0)
            vector_iter_calls.insert(it);
    }

    auto purity_cb = [&](uint32_t idx) {
        if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
            return;
        const auto &n = ctx.nodes[idx];
        if (n.type == NodeType::LocalDecl) {
            if (n.as.local_decl.ident_count == 1 && n.as.local_decl.value_count == 1) {
                uint32_t t_idx = ctx.block_statements[n.as.local_decl.first_ident];
                uint32_t v_idx = ctx.block_statements[n.as.local_decl.first_value];
                if (ctx.nodes[t_idx].type == NodeType::Identifier
                    && ctx.nodes[v_idx].type == NodeType::TableConstructor) {
                    std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    const auto &tc = ctx.nodes[v_idx].as.table_cons;
                    if (tc.count > 0 && state.known_table_lengths.find(name) == state.known_table_lengths.end()) {
                        bool _all_implicit = true;
                        for (uint32_t ei = 0; ei < tc.count; ++ei) {
                            uint32_t ek = ctx.block_statements[tc.first_item + ei * 2];
                            if (ek != 0xFFFFFFFF) {
                                _all_implicit = false;
                                break;
                            }
                        }
                        if (_all_implicit) {
                            uint32_t lv = ctx.block_statements[tc.first_item + (tc.count - 1) * 2 + 1];
                            if (lv >= ctx.nodes.size()
                                || (ctx.nodes[lv].type != NodeType::Vararg
                                    && ctx.nodes[lv].type != NodeType::CallExpression)) {
                                state.known_table_lengths[name] = tc.count;
                            }
                        }
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
                    } else if (tc.count == 0) {
                        empty_array_candidates.insert(name);
                    }
                }
            }
            for (uint32_t i = 0; i < n.as.local_decl.value_count; ++i) {
                uint32_t v = ctx.block_statements[n.as.local_decl.first_value + i];
                while (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::ParenExpression)
                    v = ctx.nodes[v].as.paren_expr.expr;
                if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::Identifier
                    && !yields_number(ctx, state, v, &known_numbers)) {
                    std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
        } else if (n.type == NodeType::Assignment) {
            for (uint32_t i = 0; i < n.as.assign.target_count; ++i)
                disq_bare(ctx.block_statements[n.as.assign.first_target + i]);
            for (uint32_t i = 0; i < n.as.assign.value_count; ++i) {
                uint32_t v = ctx.block_statements[n.as.assign.first_value + i];
                while (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::ParenExpression)
                    v = ctx.nodes[v].as.paren_expr.expr;
                if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::Identifier
                    && !yields_number(ctx, state, v, &known_numbers)) {
                    std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                    disqualified_arrays.insert(name);
                }
            }
            if (n.as.assign.target_count == 1) {
                uint32_t t_idx = ctx.block_statements[n.as.assign.first_target];
                uint32_t v_idx = ctx.block_statements[n.as.assign.first_value];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && v_idx < ctx.nodes.size()
                    && ctx.nodes[v_idx].type == NodeType::BinaryOp) {
                    const auto &bin = ctx.nodes[v_idx].as.bin_op;
                    int op = bin.op;
                    if (op == static_cast<int>(BinaryOp::Add) || op == static_cast<int>(BinaryOp::Sub)) {
                        uint32_t left = bin.left;
                        uint32_t right = bin.right;
                        if (left < ctx.nodes.size() && ctx.nodes[left].type == NodeType::Identifier) {
                            std::string_view tname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                            std::string_view lname(ctx.nodes[left].as.ident.name, ctx.nodes[left].as.ident.length);
                            if (tname == lname) {
                                if (right < ctx.nodes.size()
                                    && (ctx.nodes[right].type == NodeType::Number
                                        || ctx.nodes[right].type == NodeType::Integer)) {
                                    known_numbers.insert(tname);
                                }
                            }
                        }
                    }
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
                            auto key_depends_on_self = [&](auto &self, uint32_t nid) -> bool {
                                if (nid >= ctx.nodes.size() || _opt_visited.count(nid))
                                    return false;
                                _opt_visited.insert(nid);
                                const auto &nn = ctx.nodes[nid];
                                if (nn.type == NodeType::TableAccess) {
                                    uint32_t stbl = nn.as.table_access.table;
                                    if (stbl < ctx.nodes.size() && ctx.nodes[stbl].type == NodeType::Identifier
                                        && std::string_view(
                                               ctx.nodes[stbl].as.ident.name, ctx.nodes[stbl].as.ident.length)
                                            == tname)
                                        return true;
                                }
                                if (nn.type == NodeType::Identifier) {

                                    std::string_view vn(nn.as.ident.name, nn.as.ident.length);
                                    for (const auto &nd : ctx.nodes) {
                                        if (nd.type != NodeType::Assignment)
                                            continue;
                                        if (nd.as.assign.target_count != 1)
                                            continue;
                                        uint32_t ti = ctx.block_statements[nd.as.assign.first_target];
                                        if (ctx.nodes[ti].type != NodeType::Identifier)
                                            continue;
                                        if (std::string_view(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length)
                                            != vn)
                                            continue;
                                        uint32_t vi = ctx.block_statements[nd.as.assign.first_value];
                                        return self(self, vi);
                                    }
                                    for (const auto &nd : ctx.nodes) {
                                        if (nd.type != NodeType::LocalDecl)
                                            continue;
                                        for (uint32_t ii = 0; ii < nd.as.local_decl.ident_count; ++ii) {
                                            uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + ii];
                                            if (ctx.nodes[idi].type != NodeType::Identifier)
                                                continue;
                                            if (std::string_view(
                                                    ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length)
                                                != vn)
                                                continue;
                                            uint32_t vi = (ii < nd.as.local_decl.value_count)
                                                ? ctx.block_statements[nd.as.local_decl.first_value + ii]
                                                : 0xFFFFFFFF;
                                            if (vi != 0xFFFFFFFF)
                                                return self(self, vi);
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
                            {
                                NodeType ktype = ctx.nodes[k_idx].type;
                                if (ktype != NodeType::Identifier && ktype != NodeType::Number
                                    && ktype != NodeType::Integer) {
                                    disqualified_arrays.insert(tname);
                                }
                            }
                            uint32_t v_idx = ctx.block_statements[n.as.assign.first_value];
                            if (!yields_number(ctx, state, v_idx, &known_numbers)
                                && ctx.nodes[v_idx].type != NodeType::TrueLiteral
                                && ctx.nodes[v_idx].type != NodeType::FalseLiteral
                                && ctx.nodes[v_idx].type != NodeType::TableAccess) {
                                disqualified_arrays.insert(tname);
                            }
                        }
                    }
                }
            }
        } else if (n.type == NodeType::CallExpression) {
            uint32_t _call_target = n.as.call_expr.target;
            disq_bare(_call_target);
            if (_call_target < ctx.nodes.size() && n.as.call_expr.arg_count >= 1) {
                uint32_t _arg0 = ctx.block_statements[n.as.call_expr.first_arg];
                if (_arg0 < ctx.nodes.size() && ctx.nodes[_arg0].type == NodeType::Identifier) {
                    std::string_view _arg_name(ctx.nodes[_arg0].as.ident.name, ctx.nodes[_arg0].as.ident.length);
                    bool _is_dynamic = false;
                    if (ctx.nodes[_call_target].type == NodeType::Identifier) {
                        std::string_view _fname(
                            ctx.nodes[_call_target].as.ident.name, ctx.nodes[_call_target].as.ident.length);
                        _is_dynamic = (_fname == "setmetatable");
                    } else if (ctx.nodes[_call_target].type == NodeType::TableAccess) {
                        uint32_t _tbl = ctx.nodes[_call_target].as.table_access.table;
                        uint32_t _key = ctx.nodes[_call_target].as.table_access.key;
                        if (_tbl < ctx.nodes.size() && ctx.nodes[_tbl].type == NodeType::Identifier
                            && _key < ctx.nodes.size() && ctx.nodes[_key].type == NodeType::String) {
                            std::string_view _tbl_n(ctx.nodes[_tbl].as.ident.name, ctx.nodes[_tbl].as.ident.length);
                            std::string_view _key_n(ctx.nodes[_key].as.string.text, ctx.nodes[_key].as.string.length);
                            _is_dynamic = (_tbl_n == "table" && (_key_n == "insert" || _key_n == "remove"));
                        }
                    }
                    if (_is_dynamic)
                        state.tables_with_dynamic_length.insert(_arg_name);
                }
            }

            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) {
                uint32_t v_orig = ctx.block_statements[n.as.call_expr.first_arg + i];
                uint32_t v = v_orig;
                while (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::ParenExpression)
                    v = ctx.nodes[v].as.paren_expr.expr;
                bool vec_iter_arg = i == 0 && vector_iter_calls.count(idx) > 0 && v_orig < ctx.nodes.size()
                    && ctx.nodes[v_orig].type == NodeType::Identifier;
                if (v < ctx.nodes.size()) {
                    if (ctx.nodes[v].type == NodeType::Identifier && !vec_iter_arg) {
                        std::string_view name(ctx.nodes[v].as.ident.name, ctx.nodes[v].as.ident.length);
                        disqualified_arrays.insert(name);
                    } else if (ctx.nodes[v].type == NodeType::TableConstructor) {
                        const auto &tc = ctx.nodes[v].as.table_cons;
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
                while (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::ParenExpression)
                    v = ctx.nodes[v].as.paren_expr.expr;
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
            for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                disq_bare(ctx.block_statements[n.as.generic_for.first_iter + i]);
        } else if (n.type == NodeType::TableAccess) {
            uint32_t k_idx = n.as.table_access.key;
            disq_bare(k_idx);
            uint32_t tb_idx = n.as.table_access.table;
            std::string_view tname = bare_ident(tb_idx);
            if (!tname.empty()) {
                bool key_is_string = k_idx < ctx.nodes.size() && ctx.nodes[k_idx].type == NodeType::String;
                bool table_paren = tb_idx < ctx.nodes.size() && ctx.nodes[tb_idx].type == NodeType::ParenExpression;
                if (key_is_string || table_paren)
                    disqualified_arrays.insert(tname);
            }
        } else if (n.type == NodeType::UnaryOp) {
            disq_bare(n.as.unary_op.expr);
        } else if (n.type == NodeType::BinaryOp) {
            disq_bare(n.as.bin_op.left);
            disq_bare(n.as.bin_op.right);
        } else if (n.type == NodeType::IfStatement) {
            disq_bare(n.as.if_stmt.condition);
        } else if (n.type == NodeType::WhileStatement) {
            disq_bare(n.as.while_stmt.condition);
        } else if (n.type == NodeType::RepeatStatement) {
            disq_bare(n.as.repeat_stmt.condition);
        } else if (n.type == NodeType::ForStatement) {
            disq_bare(n.as.for_stmt.start_expr);
            disq_bare(n.as.for_stmt.limit_expr);
            if (n.as.for_stmt.step_expr != 0xFFFFFFFF)
                disq_bare(n.as.for_stmt.step_expr);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                disq_bare(ctx.block_statements[n.as.table_cons.first_item + i * 2]);
                disq_bare(ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1]);
            }
        } else if (n.type == NodeType::IntrinsicCall) {
            for (uint32_t i = 0; i < n.as.intrinsic_call.arg_count; ++i)
                disq_bare(ctx.block_statements[n.as.intrinsic_call.first_arg + i]);
        } else if (n.type == NodeType::GlobalDeclStatement) {
            for (uint32_t i = 0; i < n.as.global_decl.value_count; ++i)
                disq_bare(ctx.block_statements[n.as.global_decl.first_value + i]);
        }
    };
    traverse_node(ctx, root_node, purity_cb);

    for (auto name : pure_candidates) {
        if (disqualified_arrays.find(name) == disqualified_arrays.end()) {
            state.pure_numeric_arrays.insert(name);
        }
    }

    //------------------ Element writes with an unprovable index or nil-capable value invalidate #t folds; string-key writes never do.

    for (const auto &n : ctx.nodes) {
        if (n.type != NodeType::Assignment)
            continue;
        for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti) {
            uint32_t t = ctx.block_statements[n.as.assign.first_target + ti];
            if (t >= ctx.nodes.size() || ctx.nodes[t].type != NodeType::TableAccess)
                continue;
            uint32_t tb = ctx.nodes[t].as.table_access.table;
            uint32_t k = ctx.nodes[t].as.table_access.key;
            if (tb >= ctx.nodes.size() || ctx.nodes[tb].type != NodeType::Identifier)
                continue;
            if (k < ctx.nodes.size() && ctx.nodes[k].type == NodeType::String)
                continue;
            std::string_view tn(ctx.nodes[tb].as.ident.name, ctx.nodes[tb].as.ident.length);
            uint32_t ku = k;
            while (ku < ctx.nodes.size() && ctx.nodes[ku].type == NodeType::ParenExpression)
                ku = ctx.nodes[ku].as.paren_expr.expr;
            bool in_bounds = false;
            if (ku < ctx.nodes.size()
                && (ctx.nodes[ku].type == NodeType::Integer || ctx.nodes[ku].type == NodeType::Number)) {
                double kd = ctx.nodes[ku].type == NodeType::Integer ? static_cast<double>(ctx.nodes[ku].as.integer.val)
                                                                    : ctx.nodes[ku].as.number.val;
                auto bl = state.known_table_lengths.find(tn);
                in_bounds = bl != state.known_table_lengths.end() && kd >= 1.0 && kd <= static_cast<double>(bl->second);
            }
            uint32_t vi
                = (ti < n.as.assign.value_count) ? ctx.block_statements[n.as.assign.first_value + ti] : 0xFFFFFFFF;
            bool value_safe = false;
            if (vi < ctx.nodes.size()) {
                NodeType vt = ctx.nodes[vi].type;
                value_safe = vt != NodeType::NilLiteral && vt != NodeType::Vararg
                    && (vt == NodeType::Integer || vt == NodeType::Number || vt == NodeType::String
                        || vt == NodeType::TrueLiteral || vt == NodeType::FalseLiteral
                        || vt == NodeType::TableConstructor || vt == NodeType::FunctionDef
                        || yields_number(ctx, state, vi, &known_numbers));
            }
            if (!in_bounds || !value_safe)
                state.tables_with_dynamic_length.insert(tn);
        }
    }

    //------------------ Safety: the vector<double> representation for pure numeric arrays is only valid

    {
        std::set<std::string_view> is_pure = state.pure_numeric_arrays;
        std::set<std::string_view> violate;

        auto literal_int = [&](uint32_t idx, double &d) -> bool {
            if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
                return false;
            if (ctx.nodes[idx].type == NodeType::Integer) {
                d = static_cast<double>(ctx.nodes[idx].as.integer.val);
                return true;
            }
            if (ctx.nodes[idx].type == NodeType::Number) {
                d = ctx.nodes[idx].as.number.val;
                return true;
            }
            return false;
        };
        auto literal_ge1 = [&](uint32_t idx) -> bool {
            double d;
            return literal_int(idx, d) && d >= 1.0;
        };

        auto is_known_loop_var = [&](std::string_view nm) -> bool {
            for (const auto &nd : ctx.nodes) {
                if (nd.type != NodeType::ForStatement)
                    continue;
                uint32_t var = nd.as.for_stmt.var_ident;
                if (var >= ctx.nodes.size() || ctx.nodes[var].type != NodeType::Identifier)
                    continue;
                if (std::string_view(ctx.nodes[var].as.ident.name, ctx.nodes[var].as.ident.length) != nm)
                    continue;
                double sd = 0;
                int64_t si = 0;
                bool sint = false;
                if (!is_literal_number(ctx, nd.as.for_stmt.start_expr, sd, si, sint) || sd < 1.0)
                    continue;
                if (nd.as.for_stmt.step_expr != 0xFFFFFFFF) {
                    double td = 0;
                    int64_t ti = 0;
                    bool tint = false;
                    if (!is_literal_number(ctx, nd.as.for_stmt.step_expr, td, ti, tint) || td <= 0.0)
                        continue;
                }
                return true;
            }
            return false;
        };
        std::unordered_set<uint32_t> pos_visit;
        auto key_pos_int = [&](auto &self, uint32_t idx) -> bool {
            if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || pos_visit.count(idx))
                return false;
            pos_visit.insert(idx);
            const auto &n = ctx.nodes[idx];
            if (n.type == NodeType::Integer || n.type == NodeType::Number)
                return literal_ge1(idx);
            if (n.type == NodeType::Identifier)
                return is_known_loop_var(std::string_view(n.as.ident.name, n.as.ident.length));
            if (n.type == NodeType::BinaryOp) {
                if (n.as.bin_op.op == static_cast<int>(BinaryOp::Add))
                    return self(self, n.as.bin_op.left) && self(self, n.as.bin_op.right);
                if (n.as.bin_op.op == static_cast<int>(BinaryOp::Sub))
                    return self(self, n.as.bin_op.left) && literal_ge1(n.as.bin_op.right);
            }
            return false;
        };

        std::unordered_set<uint32_t> ub_visit;
        auto key_max_value = [&](auto &self, uint32_t idx) -> std::optional<double> {
            if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || ub_visit.count(idx))
                return std::nullopt;
            ub_visit.insert(idx);
            const auto &n = ctx.nodes[idx];
            double d;
            if (n.type == NodeType::Integer || n.type == NodeType::Number)
                return literal_int(idx, d) ? std::optional<double>(d) : std::nullopt;
            if (n.type == NodeType::Identifier) {
                std::string_view nm(n.as.ident.name, n.as.ident.length);
                for (const auto &nd : ctx.nodes) {
                    if (nd.type != NodeType::ForStatement)
                        continue;
                    uint32_t var = nd.as.for_stmt.var_ident;
                    if (var >= ctx.nodes.size() || ctx.nodes[var].type != NodeType::Identifier)
                        continue;
                    if (std::string_view(ctx.nodes[var].as.ident.name, ctx.nodes[var].as.ident.length) != nm)
                        continue;
                    double sd = 0;
                    int64_t si = 0;
                    bool sint = false;
                    if (!is_literal_number(ctx, nd.as.for_stmt.start_expr, sd, si, sint))
                        continue;
                    if (sd > 1.0)
                        continue;
                    double stepv = 1.0;
                    if (nd.as.for_stmt.step_expr != 0xFFFFFFFF) {
                        double td = 0;
                        int64_t ti = 0;
                        bool tint = false;
                        if (!is_literal_number(ctx, nd.as.for_stmt.step_expr, td, ti, tint))
                            continue;
                        stepv = td;
                    }
                    if (stepv <= 0.0)
                        continue;
                    double ld = 0;
                    int64_t li = 0;
                    bool lint = false;
                    if (!is_literal_number(ctx, nd.as.for_stmt.limit_expr, ld, li, lint))
                        return std::nullopt;
                    return std::optional<double>(ld);
                }
                return std::nullopt;
            }
            if (n.type == NodeType::BinaryOp) {
                if (n.as.bin_op.op == static_cast<int>(BinaryOp::Add)) {
                    auto l = self(self, n.as.bin_op.left);
                    if (!l)
                        return std::nullopt;
                    auto r = self(self, n.as.bin_op.right);
                    if (!r)
                        return std::nullopt;
                    return std::optional<double>(*l + *r);
                }
                if (n.as.bin_op.op == static_cast<int>(BinaryOp::Sub)) {
                    auto l = self(self, n.as.bin_op.left);
                    if (!l)
                        return std::nullopt;
                    double rd;
                    if (!literal_int(n.as.bin_op.right, rd))
                        return std::nullopt;
                    return std::optional<double>(*l - rd);
                }
            }
            return std::nullopt;
        };

        auto array_read_key = [&](auto &self, uint32_t idx) -> void {
            if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || violate.size() == is_pure.size())
                return;
            const auto &n = ctx.nodes[idx];
            if (n.type == NodeType::TableAccess) {
                uint32_t tb = n.as.table_access.table;
                uint32_t k = n.as.table_access.key;
                if (tb < ctx.nodes.size() && ctx.nodes[tb].type == NodeType::Identifier) {
                    std::string_view tn(ctx.nodes[tb].as.ident.name, ctx.nodes[tb].as.ident.length);
                    if (is_pure.count(tn)) {
                        if (!key_pos_int(key_pos_int, k))
                            violate.insert(tn);
                        else {
                            bool bounded = false;
                            auto bl = state.known_table_lengths.find(tn);
                            if (bl != state.known_table_lengths.end()) {
                                if (auto mx = key_max_value(key_max_value, k))
                                    bounded = (*mx <= static_cast<double>(bl->second));
                            }
                            if (!bounded) {
                                auto ab = array_bounds.find(std::string(tn));
                                if (ab != array_bounds.end()) {
                                    if (auto mx = key_max_value(key_max_value, k)) {
                                        double bd;
                                        if (literal_int(ab->second, bd))
                                            bounded = (*mx <= bd);
                                    }
                                }
                            }
                            if (!bounded)
                                violate.insert(tn);
                        }
                    }
                }
                self(self, tb);
                self(self, k);
                return;
            }
            if (n.type == NodeType::Assignment) {
                for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti) {
                    uint32_t t = ctx.block_statements[n.as.assign.first_target + ti];
                    if (t < ctx.nodes.size() && ctx.nodes[t].type == NodeType::TableAccess) {
                        uint32_t tb2 = ctx.nodes[t].as.table_access.table;
                        if (tb2 < ctx.nodes.size() && ctx.nodes[tb2].type == NodeType::Identifier) {
                            std::string_view tn2(ctx.nodes[tb2].as.ident.name, ctx.nodes[tb2].as.ident.length);
                            if (is_pure.count(tn2)) {
                                uint32_t k2 = ctx.nodes[t].as.table_access.key;
                                double bound = 0;
                                auto bl = state.known_table_lengths.find(tn2);
                                bool have_bound = false;
                                if (bl != state.known_table_lengths.end()) {
                                    bound = static_cast<double>(bl->second);
                                    have_bound = true;
                                } else {
                                    auto ab = array_bounds.find(std::string(tn2));
                                    if (ab != array_bounds.end() && literal_int(ab->second, bound))
                                        have_bound = true;
                                }
                                if (have_bound) {
                                    if (auto mx = key_max_value(key_max_value, k2)) {
                                        if (*mx > bound)
                                            state.tables_with_dynamic_length.insert(tn2);
                                    } else {
                                        state.tables_with_dynamic_length.insert(tn2);
                                    }
                                } else {
                                    state.tables_with_dynamic_length.insert(tn2);
                                }
                            }
                        }
                    }
                    self(self, t);
                }
                for (uint32_t vi = 0; vi < n.as.assign.value_count; ++vi)
                    self(self, ctx.block_statements[n.as.assign.first_value + vi]);
                return;
            }
            if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement) {
                uint32_t v_count
                    = (n.type == NodeType::LocalDecl) ? n.as.local_decl.value_count : n.as.global_decl.value_count;
                for (uint32_t vi = 0; vi < v_count; ++vi) {
                    uint32_t v = (n.type == NodeType::LocalDecl)
                        ? ctx.block_statements[n.as.local_decl.first_value + vi]
                        : ctx.block_statements[n.as.global_decl.first_value + vi];
                    self(self, v);
                }
                return;
            }
            if (n.type == NodeType::ReturnStatement) {
                for (uint32_t vi = 0; vi < n.as.return_stmt.value_count; ++vi)
                    self(self, ctx.block_statements[n.as.return_stmt.first_value + vi]);
                return;
            }
            if (n.type == NodeType::IfStatement) {
                self(self, n.as.if_stmt.condition);
                if (n.as.if_stmt.then_block != 0xFFFFFFFF)
                    self(self, n.as.if_stmt.then_block);
                if (n.as.if_stmt.else_block != 0xFFFFFFFF)
                    self(self, n.as.if_stmt.else_block);
                return;
            }
            if (n.type == NodeType::WhileStatement) {
                self(self, n.as.while_stmt.condition);
                self(self, n.as.while_stmt.body_block);
                return;
            }
            if (n.type == NodeType::RepeatStatement) {
                self(self, n.as.repeat_stmt.condition);
                self(self, n.as.repeat_stmt.body_block);
                return;
            }
            if (n.type == NodeType::ForStatement) {
                self(self, n.as.for_stmt.start_expr);
                self(self, n.as.for_stmt.limit_expr);
                if (n.as.for_stmt.step_expr != 0xFFFFFFFF)
                    self(self, n.as.for_stmt.step_expr);
                self(self, n.as.for_stmt.body_block);
                return;
            }
            if (n.type == NodeType::GenericForStatement) {
                for (uint32_t vi = 0; vi < n.as.generic_for.var_count; ++vi)
                    self(self, ctx.block_statements[n.as.generic_for.first_var + vi]);
                for (uint32_t ii = 0; ii < n.as.generic_for.iter_count; ++ii)
                    self(self, ctx.block_statements[n.as.generic_for.first_iter + ii]);
                self(self, n.as.generic_for.body_block);
                return;
            }
            if (n.type == NodeType::DoStatement) {
                self(self, n.as.do_stmt.body_block);
                return;
            }
            if (n.type == NodeType::FunctionDef) {
                self(self, n.as.func_def.body_block);
                return;
            }
            if (n.type == NodeType::Block) {
                for (uint32_t si = 0; si < n.as.block.count; ++si)
                    self(self, ctx.block_statements[n.as.block.first_statement + si]);
                return;
            }
            if (n.type == NodeType::BinaryOp) {
                self(self, n.as.bin_op.left);
                self(self, n.as.bin_op.right);
                return;
            }
            if (n.type == NodeType::UnaryOp) {
                self(self, n.as.unary_op.expr);
                return;
            }
            if (n.type == NodeType::CallExpression) {
                self(self, n.as.call_expr.target);
                for (uint32_t ai = 0; ai < n.as.call_expr.arg_count; ++ai)
                    self(self, ctx.block_statements[n.as.call_expr.first_arg + ai]);
                return;
            }
            if (n.type == NodeType::IntrinsicCall) {
                for (uint32_t ai = 0; ai < n.as.intrinsic_call.arg_count; ++ai)
                    self(self, ctx.block_statements[n.as.intrinsic_call.first_arg + ai]);
                return;
            }
            if (n.type == NodeType::TableConstructor) {
                for (uint32_t ei = 0; ei < n.as.table_cons.count; ++ei) {
                    self(self, ctx.block_statements[n.as.table_cons.first_item + ei * 2]);
                    self(self, ctx.block_statements[n.as.table_cons.first_item + ei * 2 + 1]);
                }
                return;
            }
        };

        array_read_key(array_read_key, root_node);
        for (auto tn : violate)
            state.pure_numeric_arrays.erase(tn);
    }
}

//------------------ pass_array_safety: safety passes over pure-array promotions
void pass_array_safety(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &disqualified_arrays = sk.disqualified_arrays;
    auto &empty_array_candidates = sk.empty_array_candidates;
    auto &array_bounds = sk.array_bounds;
    std::set<std::string_view> safety_pass_names;
    for (const auto &nm : state.pure_numeric_arrays)
        safety_pass_names.insert(nm);

    std::set<std::string_view> captured_bases;
    for (const auto &nd : ctx.nodes) {
        if (nd.type == NodeType::Identifier && nd.as.ident.is_captured) {
            captured_bases.insert(std::string_view(nd.as.ident.name, nd.as.ident.length));
        }
    }

    std::set<std::string_view> promote_ready;
    for (const auto &nd : ctx.nodes) {
        if (nd.type != NodeType::TableAccess)
            continue;
        if (nd.as.table_access.table >= ctx.nodes.size())
            continue;
        if (ctx.nodes[nd.as.table_access.table].type != NodeType::Identifier)
            continue;
        std::string_view tname(
            ctx.nodes[nd.as.table_access.table].as.ident.name, ctx.nodes[nd.as.table_access.table].as.ident.length);
        if (!empty_array_candidates.count(tname))
            continue;
        if (disqualified_arrays.count(tname))
            continue;
        if (state.reassigned_vars.count(tname))
            continue;
        if (captured_bases.count(tname))
            continue;
        uint32_t k = nd.as.table_access.key;
        if (k >= ctx.nodes.size())
            continue;
        NodeType kt = ctx.nodes[k].type;
        if (kt != NodeType::Identifier && kt != NodeType::Integer && kt != NodeType::Number)
            continue;
        promote_ready.insert(tname);
    }
    for (auto name : promote_ready) {
        state.pure_numeric_arrays.insert(name);
    }

    //------------------ Safety pass for empty-array promotions
    {
        std::set<std::string_view> new_names;
        for (const auto &nm : state.pure_numeric_arrays) {
            if (!safety_pass_names.count(nm))
                new_names.insert(nm);
        }
        if (!new_names.empty()) {
            std::set<std::string_view> is_pure2 = new_names;
            std::set<std::string_view> violate2;

            auto literal_int2 = [&](uint32_t idx, double &d) -> bool {
                if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
                    return false;
                if (ctx.nodes[idx].type == NodeType::Integer) {
                    d = static_cast<double>(ctx.nodes[idx].as.integer.val);
                    return true;
                }
                if (ctx.nodes[idx].type == NodeType::Number) {
                    d = ctx.nodes[idx].as.number.val;
                    return true;
                }
                return false;
            };
            auto literal_ge1_2 = [&](uint32_t idx) -> bool {
                double d;
                return literal_int2(idx, d) && d >= 1.0;
            };

            auto is_known_loop_var2 = [&](std::string_view nm) -> bool {
                for (const auto &nd : ctx.nodes) {
                    if (nd.type != NodeType::ForStatement)
                        continue;
                    uint32_t var = nd.as.for_stmt.var_ident;
                    if (var >= ctx.nodes.size() || ctx.nodes[var].type != NodeType::Identifier)
                        continue;
                    if (std::string_view(ctx.nodes[var].as.ident.name, ctx.nodes[var].as.ident.length) != nm)
                        continue;
                    double sd = 0;
                    int64_t si = 0;
                    bool sint = false;
                    if (!is_literal_number(ctx, nd.as.for_stmt.start_expr, sd, si, sint) || sd < 1.0)
                        continue;
                    if (nd.as.for_stmt.step_expr != 0xFFFFFFFF) {
                        double td = 0;
                        int64_t ti = 0;
                        bool tint = false;
                        if (!is_literal_number(ctx, nd.as.for_stmt.step_expr, td, ti, tint) || td <= 0.0)
                            continue;
                    }
                    return true;
                }
                return false;
            };

            std::unordered_set<uint32_t> pos_visit2;
            auto key_pos_int2 = [&](auto &self, uint32_t idx) -> bool {
                if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || pos_visit2.count(idx))
                    return false;
                pos_visit2.insert(idx);
                const auto &n = ctx.nodes[idx];
                if (n.type == NodeType::Integer || n.type == NodeType::Number)
                    return literal_ge1_2(idx);
                if (n.type == NodeType::Identifier)
                    return is_known_loop_var2(std::string_view(n.as.ident.name, n.as.ident.length));
                if (n.type == NodeType::BinaryOp) {
                    if (n.as.bin_op.op == static_cast<int>(BinaryOp::Add))
                        return self(self, n.as.bin_op.left) && self(self, n.as.bin_op.right);
                    if (n.as.bin_op.op == static_cast<int>(BinaryOp::Sub))
                        return self(self, n.as.bin_op.left) && literal_ge1_2(n.as.bin_op.right);
                }
                return false;
            };

            std::unordered_set<uint32_t> ub_visit2;
            auto key_max_value2 = [&](auto &self, uint32_t idx) -> std::optional<double> {
                if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || ub_visit2.count(idx))
                    return std::nullopt;
                ub_visit2.insert(idx);
                const auto &n = ctx.nodes[idx];
                double d;
                if (n.type == NodeType::Integer || n.type == NodeType::Number)
                    return literal_int2(idx, d) ? std::optional<double>(d) : std::nullopt;
                if (n.type == NodeType::Identifier) {
                    std::string_view nm(n.as.ident.name, n.as.ident.length);
                    for (const auto &nd : ctx.nodes) {
                        if (nd.type != NodeType::ForStatement)
                            continue;
                        uint32_t var = nd.as.for_stmt.var_ident;
                        if (var >= ctx.nodes.size() || ctx.nodes[var].type != NodeType::Identifier)
                            continue;
                        if (std::string_view(ctx.nodes[var].as.ident.name, ctx.nodes[var].as.ident.length) != nm)
                            continue;
                        double sd = 0;
                        int64_t si = 0;
                        bool sint = false;
                        if (!is_literal_number(ctx, nd.as.for_stmt.start_expr, sd, si, sint))
                            continue;
                        if (sd > 1.0)
                            continue;
                        double stepv = 1.0;
                        if (nd.as.for_stmt.step_expr != 0xFFFFFFFF) {
                            double td = 0;
                            int64_t ti = 0;
                            bool tint = false;
                            if (!is_literal_number(ctx, nd.as.for_stmt.step_expr, td, ti, tint))
                                continue;
                            stepv = td;
                        }
                        if (stepv <= 0.0)
                            continue;
                        double ld = 0;
                        int64_t li = 0;
                        bool lint = false;
                        if (!is_literal_number(ctx, nd.as.for_stmt.limit_expr, ld, li, lint))
                            return std::nullopt;
                        return std::optional<double>(ld);
                    }
                    return std::nullopt;
                }
                if (n.type == NodeType::BinaryOp) {
                    if (n.as.bin_op.op == static_cast<int>(BinaryOp::Add)) {
                        auto l = self(self, n.as.bin_op.left);
                        if (!l)
                            return std::nullopt;
                        auto r = self(self, n.as.bin_op.right);
                        if (!r)
                            return std::nullopt;
                        return std::optional<double>(*l + *r);
                    }
                    if (n.as.bin_op.op == static_cast<int>(BinaryOp::Sub)) {
                        auto l = self(self, n.as.bin_op.left);
                        if (!l)
                            return std::nullopt;
                        double rd;
                        if (!literal_int2(n.as.bin_op.right, rd))
                            return std::nullopt;
                        return std::optional<double>(*l - rd);
                    }
                }
                return std::nullopt;
            };

            auto walk2 = [&](auto &self, uint32_t idx) -> void {
                if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || violate2.size() == is_pure2.size())
                    return;
                const auto &n = ctx.nodes[idx];
                if (n.type == NodeType::TableAccess) {
                    uint32_t tb = n.as.table_access.table;
                    uint32_t k = n.as.table_access.key;
                    if (tb < ctx.nodes.size() && ctx.nodes[tb].type == NodeType::Identifier) {
                        std::string_view tn(ctx.nodes[tb].as.ident.name, ctx.nodes[tb].as.ident.length);
                        if (is_pure2.count(tn)) {
                            if (!key_pos_int2(key_pos_int2, k))
                                violate2.insert(tn);
                            else {
                                bool bounded = false;
                                auto bl = state.known_table_lengths.find(tn);
                                if (bl != state.known_table_lengths.end()) {
                                    if (auto mx = key_max_value2(key_max_value2, k))
                                        bounded = (*mx <= static_cast<double>(bl->second));
                                }
                                if (!bounded) {
                                    auto ab = array_bounds.find(std::string(tn));
                                    if (ab != array_bounds.end()) {
                                        if (auto mx = key_max_value2(key_max_value2, k)) {
                                            double bd;
                                            if (literal_int2(ab->second, bd))
                                                bounded = (*mx <= bd);
                                        }
                                    }
                                }
                                if (!bounded)
                                    violate2.insert(tn);
                            }
                        }
                    }
                    self(self, tb);
                    self(self, k);
                    return;
                }
                if (n.type == NodeType::Assignment) {
                    for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti)
                        self(self, ctx.block_statements[n.as.assign.first_target + ti]);
                    for (uint32_t vi = 0; vi < n.as.assign.value_count; ++vi)
                        self(self, ctx.block_statements[n.as.assign.first_value + vi]);
                    return;
                }
                if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement) {
                    uint32_t v_count
                        = (n.type == NodeType::LocalDecl) ? n.as.local_decl.value_count : n.as.global_decl.value_count;
                    for (uint32_t vi = 0; vi < v_count; ++vi) {
                        uint32_t v = (n.type == NodeType::LocalDecl)
                            ? ctx.block_statements[n.as.local_decl.first_value + vi]
                            : ctx.block_statements[n.as.global_decl.first_value + vi];
                        self(self, v);
                    }
                    return;
                }
                if (n.type == NodeType::ReturnStatement) {
                    for (uint32_t vi = 0; vi < n.as.return_stmt.value_count; ++vi)
                        self(self, ctx.block_statements[n.as.return_stmt.first_value + vi]);
                    return;
                }
                if (n.type == NodeType::IfStatement) {
                    self(self, n.as.if_stmt.condition);
                    if (n.as.if_stmt.then_block != 0xFFFFFFFF)
                        self(self, n.as.if_stmt.then_block);
                    if (n.as.if_stmt.else_block != 0xFFFFFFFF)
                        self(self, n.as.if_stmt.else_block);
                    return;
                }
                if (n.type == NodeType::WhileStatement) {
                    self(self, n.as.while_stmt.condition);
                    self(self, n.as.while_stmt.body_block);
                    return;
                }
                if (n.type == NodeType::RepeatStatement) {
                    self(self, n.as.repeat_stmt.condition);
                    self(self, n.as.repeat_stmt.body_block);
                    return;
                }
                if (n.type == NodeType::ForStatement) {
                    self(self, n.as.for_stmt.start_expr);
                    self(self, n.as.for_stmt.limit_expr);
                    if (n.as.for_stmt.step_expr != 0xFFFFFFFF)
                        self(self, n.as.for_stmt.step_expr);
                    self(self, n.as.for_stmt.body_block);
                    return;
                }
                if (n.type == NodeType::GenericForStatement) {
                    for (uint32_t vi = 0; vi < n.as.generic_for.var_count; ++vi)
                        self(self, ctx.block_statements[n.as.generic_for.first_var + vi]);
                    for (uint32_t ii = 0; ii < n.as.generic_for.iter_count; ++ii)
                        self(self, ctx.block_statements[n.as.generic_for.first_iter + ii]);
                    self(self, n.as.generic_for.body_block);
                    return;
                }
                if (n.type == NodeType::DoStatement) {
                    self(self, n.as.do_stmt.body_block);
                    return;
                }
                if (n.type == NodeType::FunctionDef) {
                    self(self, n.as.func_def.body_block);
                    return;
                }
                if (n.type == NodeType::Block) {
                    for (uint32_t si = 0; si < n.as.block.count; ++si)
                        self(self, ctx.block_statements[n.as.block.first_statement + si]);
                    return;
                }
                if (n.type == NodeType::BinaryOp) {
                    self(self, n.as.bin_op.left);
                    self(self, n.as.bin_op.right);
                    return;
                }
                if (n.type == NodeType::UnaryOp) {
                    self(self, n.as.unary_op.expr);
                    return;
                }
                if (n.type == NodeType::CallExpression) {
                    self(self, n.as.call_expr.target);
                    for (uint32_t ai = 0; ai < n.as.call_expr.arg_count; ++ai)
                        self(self, ctx.block_statements[n.as.call_expr.first_arg + ai]);
                    return;
                }
                if (n.type == NodeType::IntrinsicCall) {
                    for (uint32_t ai = 0; ai < n.as.intrinsic_call.arg_count; ++ai)
                        self(self, ctx.block_statements[n.as.intrinsic_call.first_arg + ai]);
                    return;
                }
                if (n.type == NodeType::TableConstructor) {
                    for (uint32_t ei = 0; ei < n.as.table_cons.count; ++ei) {
                        self(self, ctx.block_statements[n.as.table_cons.first_item + ei * 2]);
                        self(self, ctx.block_statements[n.as.table_cons.first_item + ei * 2 + 1]);
                    }
                    return;
                }
            };

            walk2(walk2, root_node);
            for (auto tn : violate2)
                state.pure_numeric_arrays.erase(tn);
        }
    }

    //------------------ int_numeric_arrays: promoted arrays whose every value source is statically

    {
        std::set<std::string_view> int_arrays;
        for (const auto &nm : state.pure_numeric_arrays)
            int_arrays.insert(nm);
        std::set<std::string_view> violate_int;
        auto walk_int = [&](auto &self, uint32_t idx) -> void {
            if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size() || violate_int.size() == int_arrays.size())
                return;
            const auto &n = ctx.nodes[idx];
            if (n.type == NodeType::Assignment) {
                for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti) {
                    uint32_t t = ctx.block_statements[n.as.assign.first_target + ti];
                    if (t < ctx.nodes.size() && ctx.nodes[t].type == NodeType::TableAccess) {
                        uint32_t tb = ctx.nodes[t].as.table_access.table;
                        if (tb < ctx.nodes.size() && ctx.nodes[tb].type == NodeType::Identifier) {
                            std::string_view tn(ctx.nodes[tb].as.ident.name, ctx.nodes[tb].as.ident.length);
                            if (int_arrays.count(tn) && !violate_int.count(tn)) {
                                uint32_t v = (n.as.assign.value_count == 1)
                                    ? ctx.block_statements[n.as.assign.first_value]
                                    : 0xFFFFFFFF;
                                if (v == 0xFFFFFFFF || !is_purely_integer_expr(ctx, state, v))
                                    violate_int.insert(tn);
                            }
                        }
                    }
                }
                for (uint32_t vi = 0; vi < n.as.assign.value_count; ++vi)
                    self(self, ctx.block_statements[n.as.assign.first_value + vi]);
                return;
            }
            if (n.type == NodeType::TableConstructor) {
                for (uint32_t ei = 0; ei < n.as.table_cons.count; ++ei) {
                    uint32_t ki = ctx.block_statements[n.as.table_cons.first_item + ei * 2];
                    uint32_t vi = ctx.block_statements[n.as.table_cons.first_item + ei * 2 + 1];
                    self(self, ki);
                    self(self, vi);
                }
                return;
            }
            if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement) {
                uint32_t v_count
                    = (n.type == NodeType::LocalDecl) ? n.as.local_decl.value_count : n.as.global_decl.value_count;
                for (uint32_t vi = 0; vi < v_count; ++vi) {
                    uint32_t v = (n.type == NodeType::LocalDecl)
                        ? ctx.block_statements[n.as.local_decl.first_value + vi]
                        : ctx.block_statements[n.as.global_decl.first_value + vi];
                    self(self, v);
                }
                return;
            }
            if (n.type == NodeType::ReturnStatement) {
                for (uint32_t vi = 0; vi < n.as.return_stmt.value_count; ++vi)
                    self(self, ctx.block_statements[n.as.return_stmt.first_value + vi]);
                return;
            }
            if (n.type == NodeType::IfStatement) {
                self(self, n.as.if_stmt.condition);
                if (n.as.if_stmt.then_block != 0xFFFFFFFF)
                    self(self, n.as.if_stmt.then_block);
                if (n.as.if_stmt.else_block != 0xFFFFFFFF)
                    self(self, n.as.if_stmt.else_block);
                return;
            }
            if (n.type == NodeType::WhileStatement) {
                self(self, n.as.while_stmt.condition);
                self(self, n.as.while_stmt.body_block);
                return;
            }
            if (n.type == NodeType::RepeatStatement) {
                self(self, n.as.repeat_stmt.condition);
                self(self, n.as.repeat_stmt.body_block);
                return;
            }
            if (n.type == NodeType::ForStatement) {
                self(self, n.as.for_stmt.start_expr);
                self(self, n.as.for_stmt.limit_expr);
                if (n.as.for_stmt.step_expr != 0xFFFFFFFF)
                    self(self, n.as.for_stmt.step_expr);
                self(self, n.as.for_stmt.body_block);
                return;
            }
            if (n.type == NodeType::GenericForStatement) {
                for (uint32_t vi = 0; vi < n.as.generic_for.var_count; ++vi)
                    self(self, ctx.block_statements[n.as.generic_for.first_var + vi]);
                for (uint32_t ii = 0; ii < n.as.generic_for.iter_count; ++ii)
                    self(self, ctx.block_statements[n.as.generic_for.first_iter + ii]);
                self(self, n.as.generic_for.body_block);
                return;
            }
            if (n.type == NodeType::DoStatement) {
                self(self, n.as.do_stmt.body_block);
                return;
            }
            if (n.type == NodeType::FunctionDef) {
                self(self, n.as.func_def.body_block);
                return;
            }
            if (n.type == NodeType::Block) {
                for (uint32_t si = 0; si < n.as.block.count; ++si)
                    self(self, ctx.block_statements[n.as.block.first_statement + si]);
                return;
            }
            if (n.type == NodeType::BinaryOp) {
                self(self, n.as.bin_op.left);
                self(self, n.as.bin_op.right);
                return;
            }
            if (n.type == NodeType::UnaryOp) {
                self(self, n.as.unary_op.expr);
                return;
            }
            if (n.type == NodeType::CallExpression) {
                self(self, n.as.call_expr.target);
                for (uint32_t ai = 0; ai < n.as.call_expr.arg_count; ++ai)
                    self(self, ctx.block_statements[n.as.call_expr.first_arg + ai]);
                return;
            }
            if (n.type == NodeType::IntrinsicCall) {
                for (uint32_t ai = 0; ai < n.as.intrinsic_call.arg_count; ++ai)
                    self(self, ctx.block_statements[n.as.intrinsic_call.first_arg + ai]);
                return;
            }
        };
        walk_int(walk_int, root_node);
        for (auto tn : violate_int)
            int_arrays.erase(tn);
        for (const auto &nm : int_arrays)
            state.int_numeric_arrays.insert(nm);
    }
}

//------------------ pass_table_fields: numeric table field typing and zero-index proof
void pass_table_fields(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &known_numbers = sk.known_numbers;
    auto &disqualified = sk.disqualified;
    state.numeric_table_fields.clear();
    state.zero_index_tables.clear();

    //------------------ Names reassigned after declaration must not carry constructor field typing
    std::unordered_map<uint32_t, std::set<std::string_view>> reassigned_names;
    for (uint32_t idx = 0; idx < ctx.nodes.size(); ++idx) {
        const auto &nn = ctx.nodes[idx];
        if (nn.type != NodeType::Assignment)
            continue;
        uint32_t owner = owner_of_node(state, idx);
        for (uint32_t ti = 0; ti < nn.as.assign.target_count; ++ti) {
            uint32_t tgt = ctx.block_statements[nn.as.assign.first_target + ti];
            if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier)
                reassigned_names[owner].insert(
                    std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length));
        }
    }
    //------------------ zero-index proof: fresh {__index=0}-metatable locals never escaping except as a table position or same-function return.
    std::vector<bool> zi_table_pos(ctx.nodes.size(), false);
    std::vector<bool> zi_return_pos(ctx.nodes.size(), false);
    for (uint32_t zi_i = 0; zi_i < ctx.nodes.size(); ++zi_i) {
        const auto &zi_n = ctx.nodes[zi_i];
        if (zi_n.type == NodeType::TableAccess && zi_n.as.table_access.table < ctx.nodes.size())
            zi_table_pos[zi_n.as.table_access.table] = true;
        else if (zi_n.type == NodeType::ReturnStatement) {
            for (uint32_t zi_r = 0; zi_r < zi_n.as.return_stmt.value_count; ++zi_r) {
                uint32_t zi_rv = ctx.block_statements[zi_n.as.return_stmt.first_value + zi_r];
                if (zi_rv < ctx.nodes.size())
                    zi_return_pos[zi_rv] = true;
            }
        }
    }
    auto zi_zero_lit = [&](uint32_t vi) -> bool {
        if (vi >= ctx.nodes.size())
            return false;
        if (ctx.nodes[vi].type == NodeType::Integer)
            return ctx.nodes[vi].as.integer.val == 0;
        if (ctx.nodes[vi].type == NodeType::Number)
            return ctx.nodes[vi].as.number.val == 0.0;
        return false;
    };
    auto zi_index_zero = [&](uint32_t vi) -> bool {
        if (vi >= ctx.nodes.size())
            return false;
        if (ctx.nodes[vi].type != NodeType::FunctionDef)
            return zi_zero_lit(vi);
        const auto &zi_fd = ctx.nodes[vi].as.func_def;
        if (zi_fd.param_count != 0 || zi_fd.is_vararg)
            return false;
        if (zi_fd.body_block >= ctx.nodes.size() || ctx.nodes[zi_fd.body_block].type != NodeType::Block)
            return false;
        const auto &zi_bd = ctx.nodes[zi_fd.body_block].as.block;
        if (zi_bd.count != 1)
            return false;
        uint32_t zi_st = ctx.block_statements[zi_bd.first_statement];
        if (zi_st >= ctx.nodes.size() || ctx.nodes[zi_st].type != NodeType::ReturnStatement)
            return false;
        const auto &zi_rn = ctx.nodes[zi_st].as.return_stmt;
        return zi_rn.value_count == 1 && zi_zero_lit(ctx.block_statements[zi_rn.first_value]);
    };
    for (uint32_t zi_li = 0; zi_li < ctx.nodes.size(); ++zi_li) {
        const auto &node = ctx.nodes[zi_li];
        if (node.type != NodeType::LocalDecl)
            continue;
        if (node.as.local_decl.ident_count != 1 || node.as.local_decl.value_count != 1)
            continue;
        uint32_t id_idx = ctx.block_statements[node.as.local_decl.first_ident];
        uint32_t val_idx = ctx.block_statements[node.as.local_decl.first_value];
        if (id_idx >= ctx.nodes.size() || ctx.nodes[id_idx].type != NodeType::Identifier)
            continue;
        if (val_idx >= ctx.nodes.size() || ctx.nodes[val_idx].type != NodeType::CallExpression)
            continue;
        const auto &vc = ctx.nodes[val_idx];
        if (vc.as.call_expr.arg_count != 2)
            continue;
        uint32_t zi_tgt = vc.as.call_expr.target;
        if (zi_tgt >= ctx.nodes.size() || ctx.nodes[zi_tgt].type != NodeType::Identifier
            || !ctx.nodes[zi_tgt].as.ident.is_global)
            continue;
        std::string_view zi_fn(ctx.nodes[zi_tgt].as.ident.name, ctx.nodes[zi_tgt].as.ident.length);
        if (zi_fn != "setmetatable")
            continue;
        uint32_t zi_a0 = ctx.block_statements[vc.as.call_expr.first_arg];
        uint32_t zi_a1 = ctx.block_statements[vc.as.call_expr.first_arg + 1];
        if (zi_a0 >= ctx.nodes.size() || ctx.nodes[zi_a0].type != NodeType::TableConstructor)
            continue;
        if (zi_a1 >= ctx.nodes.size() || ctx.nodes[zi_a1].type != NodeType::TableConstructor)
            continue;
        const auto &zi_mt = ctx.nodes[zi_a1].as.table_cons;
        if (zi_mt.count != 1)
            continue;
        uint32_t zi_k = ctx.block_statements[zi_mt.first_item];
        if (zi_k >= ctx.nodes.size() || ctx.nodes[zi_k].type != NodeType::String)
            continue;
        std::string_view zi_key(ctx.nodes[zi_k].as.string.text, ctx.nodes[zi_k].as.string.length);
        if (zi_key != "__index")
            continue;
        if (!zi_index_zero(ctx.block_statements[zi_mt.first_item + 1]))
            continue;
        std::string_view zi_nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
        uint32_t zi_own = owner_of_node(state, zi_li);
        bool zi_escapes = false;
        for (uint32_t zi_u = 0; zi_u < ctx.nodes.size() && !zi_escapes; ++zi_u) {
            if (ctx.nodes[zi_u].type != NodeType::Identifier || zi_u == id_idx)
                continue;
            if (std::string_view(ctx.nodes[zi_u].as.ident.name, ctx.nodes[zi_u].as.ident.length) != zi_nm)
                continue;
            if (zi_table_pos[zi_u])
                continue;
            if (zi_return_pos[zi_u] && owner_of_node(state, zi_u) == zi_own)
                continue;
            zi_escapes = true;
        }
        if (zi_escapes)
            continue;
        state.zero_index_tables.insert({ zi_own, zi_nm });
    }
    for (const auto &node : ctx.nodes) {
        if (node.type != NodeType::LocalDecl)
            continue;
        if (node.as.local_decl.ident_count != 1 || node.as.local_decl.value_count != 1)
            continue;
        uint32_t id_idx = ctx.block_statements[node.as.local_decl.first_ident];
        uint32_t val_idx = ctx.block_statements[node.as.local_decl.first_value];
        if (ctx.nodes[id_idx].type != NodeType::Identifier)
            continue;
        std::string_view nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
        if (ctx.nodes[val_idx].type != NodeType::TableConstructor)
            continue;
        auto rit_a = reassigned_names.find(owner_of_node(state, id_idx));
        if (rit_a != reassigned_names.end() && rit_a->second.count(nm))
            continue;
        const auto &tc = ctx.nodes[val_idx].as.table_cons;
        if (tc.count == 0)
            continue;

        std::map<std::string_view, bool> field_numeric;
        std::set<std::string_view> all_fields;
        bool first = true;
        bool valid = true;
        for (uint32_t ei = 0; ei < tc.count && valid; ++ei) {
            uint32_t ev = ctx.block_statements[tc.first_item + ei * 2 + 1];
            if (ctx.nodes[ev].type != NodeType::TableConstructor) {
                valid = false;
                break;
            }
            const auto &inner = ctx.nodes[ev].as.table_cons;
            std::set<std::string_view> entry_fields;
            for (uint32_t fi = 0; fi < inner.count; ++fi) {
                uint32_t fk = ctx.block_statements[inner.first_item + fi * 2];
                uint32_t fv = ctx.block_statements[inner.first_item + fi * 2 + 1];
                if (fk >= ctx.nodes.size() || ctx.nodes[fk].type != NodeType::String)
                    continue;
                std::string_view fname(ctx.nodes[fk].as.string.text, ctx.nodes[fk].as.string.length);
                entry_fields.insert(fname);
                bool is_num = yields_number(ctx, state, fv, nullptr);
                if (!is_num && ctx.nodes[fv].type == NodeType::Identifier) {

                    std::string_view vname(ctx.nodes[fv].as.ident.name, ctx.nodes[fv].as.ident.length);
                    for (const auto &nd : ctx.nodes) {
                        if (nd.type != NodeType::LocalDecl)
                            continue;
                        for (uint32_t ii = 0; ii < nd.as.local_decl.ident_count; ++ii) {
                            uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + ii];
                            if (ctx.nodes[idi].type != NodeType::Identifier)
                                continue;
                            if (std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length) != vname)
                                continue;
                            uint32_t vi = (ii < nd.as.local_decl.value_count)
                                ? ctx.block_statements[nd.as.local_decl.first_value + ii]
                                : 0xFFFFFFFF;
                            if (vi != 0xFFFFFFFF && yields_number(ctx, state, vi, &known_numbers)) {
                                is_num = true;
                                break;
                            }
                        }
                        if (is_num)
                            break;
                    }
                }
                if (first)
                    field_numeric[fname] = is_num;
                else if (field_numeric.count(fname) && field_numeric[fname] && !is_num)
                    field_numeric[fname] = false;
            }
            if (first)
                all_fields = entry_fields;
            else {
                std::set<std::string_view> intersect;
                for (auto &f : all_fields)
                    if (entry_fields.count(f))
                        intersect.insert(f);
                all_fields = intersect;
            }
            first = false;
        }
        if (valid && !all_fields.empty()) {
            std::set<std::string_view> numeric_fields;
            for (auto &f : all_fields) {
                auto it = field_numeric.find(f);
                if (it != field_numeric.end() && it->second)
                    numeric_fields.insert(it->first);
            }
            if (!numeric_fields.empty()) {
                state.numeric_table_fields[{ owner_of_node(state, id_idx), nm }] = numeric_fields;

                for (auto &fld : numeric_fields) {
                    if (state.string_pool_index.find(fld) == state.string_pool_index.end()) {
                        state.string_pool_index[fld] = state.string_pool.size();
                        state.string_pool.push_back(fld);
                    }
                }
            }
        }
    }
    for (const auto &node : ctx.nodes) {
        if (node.type != NodeType::LocalDecl)
            continue;
        if (node.as.local_decl.ident_count != 1 || node.as.local_decl.value_count != 1)
            continue;
        uint32_t id_idx = ctx.block_statements[node.as.local_decl.first_ident];
        uint32_t val_idx = ctx.block_statements[node.as.local_decl.first_value];
        if (id_idx >= ctx.nodes.size() || val_idx >= ctx.nodes.size())
            continue;
        if (ctx.nodes[id_idx].type != NodeType::Identifier)
            continue;
        std::string_view nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
        if (ctx.nodes[val_idx].type != NodeType::TableConstructor)
            continue;
        auto rit_b = reassigned_names.find(owner_of_node(state, id_idx));
        if (rit_b != reassigned_names.end() && rit_b->second.count(nm))
            continue;
        if (state.numeric_table_fields.count({ owner_of_node(state, id_idx), nm }))
            continue;
        const auto &tc = ctx.nodes[val_idx].as.table_cons;
        if (tc.count == 0)
            continue;

        std::set<std::string_view> numeric_fields;
        bool valid = true;
        for (uint32_t ei = 0; ei < tc.count && valid; ++ei) {
            uint32_t fk = ctx.block_statements[tc.first_item + ei * 2];
            uint32_t fv = ctx.block_statements[tc.first_item + ei * 2 + 1];
            if (fk >= ctx.nodes.size() || fv >= ctx.nodes.size()) {
                valid = false;
                break;
            }
            if (ctx.nodes[fk].type == NodeType::String) {
                std::string_view fname(ctx.nodes[fk].as.string.text, ctx.nodes[fk].as.string.length);
                if (yields_number(ctx, state, fv, &known_numbers))
                    numeric_fields.insert(fname);
            } else if (!yields_number(ctx, state, fv, &known_numbers)) {
                valid = false;
            }
        }

        if (valid && !numeric_fields.empty()) {
            state.numeric_table_fields[{ owner_of_node(state, id_idx), nm }] = numeric_fields;
            for (auto &fld : numeric_fields) {
                if (state.string_pool_index.find(fld) == state.string_pool_index.end()) {
                    state.string_pool_index[fld] = state.string_pool.size();
                    state.string_pool.push_back(fld);
                }
            }
        }
    }
    std::unordered_map<std::string_view, std::vector<uint32_t>> local_decl_by_pname;
    local_decl_by_pname.reserve(1024);
    for (uint32_t idx = 0; idx < ctx.nodes.size(); ++idx) {
        const auto &nd2 = ctx.nodes[idx];
        if (nd2.type != NodeType::LocalDecl)
            continue;
        if (nd2.as.local_decl.ident_count != 1 || nd2.as.local_decl.value_count != 1)
            continue;
        uint32_t id_idx = ctx.block_statements[nd2.as.local_decl.first_ident];
        uint32_t val_idx = ctx.block_statements[nd2.as.local_decl.first_value];
        if (id_idx >= ctx.nodes.size() || val_idx >= ctx.nodes.size())
            continue;
        if (ctx.nodes[id_idx].type != NodeType::Identifier)
            continue;
        if (ctx.nodes[val_idx].type != NodeType::TableAccess)
            continue;
        uint32_t tbl = ctx.nodes[val_idx].as.table_access.table;
        if (tbl >= ctx.nodes.size() || ctx.nodes[tbl].type != NodeType::Identifier)
            continue;
        std::string_view pname(ctx.nodes[tbl].as.ident.name, ctx.nodes[tbl].as.ident.length);
        local_decl_by_pname[pname].push_back(idx);
    }
    std::unordered_map<std::string_view, std::vector<uint32_t>> binary_op_by_sname;
    binary_op_by_sname.reserve(1024);
    for (uint32_t idx = 0; idx < ctx.nodes.size(); ++idx) {
        const auto &bn = ctx.nodes[idx];
        if (bn.type != NodeType::BinaryOp)
            continue;
        std::function<void(uint32_t, std::vector<std::string_view> &)> collect_snames
            = [&](uint32_t side, std::vector<std::string_view> &out) {
                  if (side >= ctx.nodes.size())
                      return;
                  const auto &sn = ctx.nodes[side];
                  if (sn.type == NodeType::TableAccess) {
                      uint32_t stbl = sn.as.table_access.table;
                      if (stbl < ctx.nodes.size() && ctx.nodes[stbl].type == NodeType::Identifier) {
                          if (sn.as.table_access.key < ctx.nodes.size()
                              && ctx.nodes[sn.as.table_access.key].type == NodeType::String) {
                              std::string_view sname(ctx.nodes[stbl].as.ident.name, ctx.nodes[stbl].as.ident.length);
                              out.push_back(sname);
                          }
                      }
                  } else if (sn.type == NodeType::ParenExpression) {
                      collect_snames(sn.as.paren_expr.expr, out);
                  }
              };
        std::vector<std::string_view> snames;
        collect_snames(bn.as.bin_op.left, snames);
        collect_snames(bn.as.bin_op.right, snames);
        for (auto &sname : snames) {
            binary_op_by_sname[sname].push_back(idx);
        }
    }

    for (uint32_t nd_idx = 0; nd_idx < ctx.nodes.size(); ++nd_idx) {
        const auto &nd = ctx.nodes[nd_idx];
        if (nd.type != NodeType::FunctionDef)
            continue;
        std::set<std::string_view> func_params;
        for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
            uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
            if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier) {
                func_params.insert(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
            }
        }
        if (func_params.empty())
            continue;
        std::map<std::string_view, std::string_view> local_to_param;
        for (auto &pname : func_params) {
            auto it = local_decl_by_pname.find(pname);
            if (it == local_decl_by_pname.end())
                continue;
            for (uint32_t scan_idx : it->second) {
                const auto &scan = ctx.nodes[scan_idx];
                if (scan.type != NodeType::LocalDecl)
                    continue;
                if (scan.as.local_decl.ident_count != 1 || scan.as.local_decl.value_count != 1)
                    continue;
                uint32_t id_idx = ctx.block_statements[scan.as.local_decl.first_ident];
                uint32_t val_idx = ctx.block_statements[scan.as.local_decl.first_value];
                if (id_idx >= ctx.nodes.size() || val_idx >= ctx.nodes.size())
                    continue;
                if (ctx.nodes[id_idx].type != NodeType::Identifier)
                    continue;
                if (ctx.nodes[val_idx].type != NodeType::TableAccess)
                    continue;
                uint32_t tbl = ctx.nodes[val_idx].as.table_access.table;
                if (tbl >= ctx.nodes.size() || ctx.nodes[tbl].type != NodeType::Identifier)
                    continue;
                std::string_view pname(ctx.nodes[tbl].as.ident.name, ctx.nodes[tbl].as.ident.length);
                if (!func_params.count(pname))
                    continue;
                if (owner_of_node(state, scan_idx) != nd_idx)
                    continue;
                if (state.numeric_table_fields.count({ nd_idx, pname }))
                    continue;
                std::string_view lname(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                local_to_param[lname] = pname;
            }
        }
        if (local_to_param.empty())
            continue;
        std::map<std::string_view, std::set<std::string_view>> param_arith_fields;
        for (auto &kv : local_to_param) {
            auto it2 = binary_op_by_sname.find(kv.first);
            if (it2 == binary_op_by_sname.end())
                continue;
            for (uint32_t bn_idx : it2->second) {
                if (owner_of_node(state, bn_idx) != nd_idx)
                    continue;
                const auto &bn = ctx.nodes[bn_idx];
                if (bn.type != NodeType::BinaryOp)
                    continue;
                std::function<void(uint32_t)> check_side;
                check_side = [&](uint32_t side_idx) {
                    if (side_idx >= ctx.nodes.size())
                        return;
                    const auto &sn = ctx.nodes[side_idx];
                    if (sn.type == NodeType::TableAccess) {
                        uint32_t stbl = sn.as.table_access.table;
                        if (stbl < ctx.nodes.size() && ctx.nodes[stbl].type == NodeType::Identifier) {
                            std::string_view sname(ctx.nodes[stbl].as.ident.name, ctx.nodes[stbl].as.ident.length);
                            auto it = local_to_param.find(sname);
                            if (it != local_to_param.end()) {
                                if (sn.as.table_access.key < ctx.nodes.size()
                                    && ctx.nodes[sn.as.table_access.key].type == NodeType::String) {
                                    std::string_view fname(ctx.nodes[sn.as.table_access.key].as.string.text,
                                        ctx.nodes[sn.as.table_access.key].as.string.length);
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
        }
        for (auto &[pn, fields] : param_arith_fields) {
            auto rit_c1 = reassigned_names.find(nd_idx);
            bool pn_reassigned = rit_c1 != reassigned_names.end() && rit_c1->second.count(pn);
            if (!fields.empty() && !pn_reassigned && !state.numeric_table_fields.count({ nd_idx, pn })) {
                state.numeric_table_fields[{ nd_idx, pn }] = fields;
                for (auto &fld : fields) {
                    if (state.string_pool_index.find(fld) == state.string_pool_index.end()) {
                        state.string_pool_index[fld] = state.string_pool.size();
                        state.string_pool.push_back(fld);
                    }
                }
            }
        }
        for (auto &[ln, pn] : local_to_param) {
            auto it = param_arith_fields.find(pn);
            if (it != param_arith_fields.end() && !it->second.empty()) {
                auto rit_c2 = reassigned_names.find(nd_idx);
                bool ln_reassigned = rit_c2 != reassigned_names.end() && rit_c2->second.count(ln);
                if (!ln_reassigned && !state.numeric_table_fields.count({ nd_idx, ln })) {
                    state.numeric_table_fields[{ nd_idx, ln }] = it->second;
                }
            }
        }
    }
    {

        int safety2 = 100;
        bool changed2;
        do {
            changed2 = false;
            if (--safety2 <= 0)
                break;
            for (const auto &node : ctx.nodes) {
                if (node.type == NodeType::LocalDecl || node.type == NodeType::GlobalDeclStatement) {
                    if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard)
                        continue;
                    uint32_t ic = (node.type == NodeType::LocalDecl) ? node.as.local_decl.ident_count
                                                                     : node.as.global_decl.ident_count;
                    uint32_t fi = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_ident
                                                                     : node.as.global_decl.first_ident;
                    uint32_t fv = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value
                                                                     : node.as.global_decl.first_value;
                    uint32_t vc = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count
                                                                     : node.as.global_decl.value_count;
                    for (uint32_t ii = 0; ii < ic; ++ii) {
                        uint32_t idi = ctx.block_statements[fi + ii];
                        std::string_view nm(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                        uint32_t vi = (ii < vc) ? ctx.block_statements[fv + ii] : 0xFFFFFFFF;
                        if (disqualified.count(nm)) {
                            if (known_numbers.erase(nm))
                                changed2 = true;
                        } else if (ctx.nodes[idi].as.ident.is_captured || ctx.nodes[idi].as.ident.is_global) {
                            disqualified.insert(nm);
                            if (known_numbers.erase(nm))
                                changed2 = true;
                        } else if (yields_number(ctx, state, vi, &known_numbers)) {
                            if (known_numbers.insert(nm).second)
                                changed2 = true;
                        } else {
                            disqualified.insert(nm);
                            if (known_numbers.erase(nm))
                                changed2 = true;
                        }
                    }
                } else if (node.type == NodeType::Assignment) {
                    for (uint32_t ii = 0; ii < node.as.assign.target_count; ++ii) {
                        uint32_t ti = ctx.block_statements[node.as.assign.first_target + ii];
                        const auto &tn = ctx.nodes[ti];
                        if (tn.type == NodeType::Identifier) {
                            std::string_view nm(tn.as.ident.name, tn.as.ident.length);
                            uint32_t vi = (ii < node.as.assign.value_count)
                                ? ctx.block_statements[node.as.assign.first_value + ii]
                                : 0xFFFFFFFF;
                            if (disqualified.count(nm)) {
                                if (known_numbers.erase(nm))
                                    changed2 = true;
                            } else if (tn.as.ident.is_global) {
                                disqualified.insert(nm);
                                if (known_numbers.erase(nm))
                                    changed2 = true;
                            } else if (yields_number(ctx, state, vi, &known_numbers)) {
                                if (!disqualified.count(nm) && known_numbers.insert(nm).second)
                                    changed2 = true;
                            } else {
                                disqualified.insert(nm);
                                if (known_numbers.erase(nm))
                                    changed2 = true;
                            }
                        }
                    }
                } else if (node.type == NodeType::GenericForStatement) {
                    for (uint32_t ii = 0; ii < node.as.generic_for.var_count; ++ii) {
                        uint32_t vi = ctx.block_statements[node.as.generic_for.first_var + ii];
                        if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::Identifier) {
                            std::string_view nm(ctx.nodes[vi].as.ident.name, ctx.nodes[vi].as.ident.length);
                            disqualified.insert(nm);
                            if (known_numbers.erase(nm))
                                changed2 = true;
                        }
                    }
                }
            }
        } while (changed2);
    }
}

//------------------ pass_arena: escaping variables and arena-safe table sizing
void pass_arena(const ASTContext &ctx, AnalysisState &state, uint32_t root_node) {
    state.escaping_vars.clear();
    state.arena_safe_table_nodes.clear();
    state.arena_table_sizes.clear();

    for (uint32_t fi = 0; fi < ctx.nodes.size(); ++fi) {
        const auto &fn = ctx.nodes[fi];
        if (fn.type != NodeType::FunctionDef)
            continue;
        uint32_t body = fn.as.func_def.body_block;
        if (body == 0xFFFFFFFF || body >= ctx.nodes.size())
            continue;

        struct LocalTable {
            std::string_view name;
            uint32_t decl_node;
            uint32_t ctor_node;
        };

        std::vector<LocalTable> local_tables;

        std::function<void(uint32_t)> walk_body = [&](uint32_t block_idx) {
            if (block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size())
                return;
            const auto &blk = ctx.nodes[block_idx];
            if (blk.type != NodeType::Block)
                return;
            for (uint32_t si = 0; si < blk.as.block.count; ++si) {
                uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + si];
                if (stmt >= ctx.nodes.size())
                    continue;
                const auto &sn = ctx.nodes[stmt];
                if (sn.type == NodeType::LocalDecl) {
                    for (uint32_t li = 0; li < sn.as.local_decl.ident_count; ++li) {
                        uint32_t id_idx = ctx.block_statements[sn.as.local_decl.first_ident + li];
                        if (id_idx >= ctx.nodes.size() || ctx.nodes[id_idx].type != NodeType::Identifier)
                            continue;
                        if (ctx.nodes[id_idx].as.ident.is_captured || ctx.nodes[id_idx].as.ident.is_global)
                            continue;
                        std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                        if (li < sn.as.local_decl.value_count) {
                            uint32_t val_idx = ctx.block_statements[sn.as.local_decl.first_value + li];
                            if (val_idx < ctx.nodes.size() && ctx.nodes[val_idx].type == NodeType::TableConstructor) {
                                local_tables.push_back({ name, stmt, val_idx });
                            }
                        }
                    }
                } else if (sn.type == NodeType::Block) {
                    walk_body(stmt);
                } else if (sn.type == NodeType::IfStatement) {
                    walk_body(sn.as.if_stmt.then_block);
                    walk_body(sn.as.if_stmt.else_block);
                } else if (sn.type == NodeType::WhileStatement) {
                    walk_body(sn.as.while_stmt.body_block);
                } else if (sn.type == NodeType::RepeatStatement) {
                    walk_body(sn.as.repeat_stmt.body_block);
                } else if (sn.type == NodeType::ForStatement) {
                    walk_body(sn.as.for_stmt.body_block);
                } else if (sn.type == NodeType::GenericForStatement) {
                    walk_body(sn.as.generic_for.body_block);
                } else if (sn.type == NodeType::FunctionDef) {
                } else if (sn.type == NodeType::DoStatement) {
                    walk_body(sn.as.do_stmt.body_block);
                }
            }
        };
        walk_body(body);

        if (local_tables.empty())
            continue;

        for (auto &lt : local_tables) {
            bool escapes = false;

            std::function<void(uint32_t)> check_escape = [&](uint32_t block_idx) {
                if (escapes || block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size())
                    return;
                const auto &blk = ctx.nodes[block_idx];
                if (blk.type != NodeType::Block)
                    return;
                for (uint32_t si = 0; si < blk.as.block.count; ++si) {
                    if (escapes)
                        return;
                    uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + si];
                    if (stmt >= ctx.nodes.size())
                        continue;
                    const auto &sn = ctx.nodes[stmt];
                    std::function<bool(uint32_t)> refs_var = [&](uint32_t n_idx) -> bool {
                        if (n_idx >= ctx.nodes.size())
                            return false;
                        const auto &n = ctx.nodes[n_idx];

                        if (n.type == NodeType::Identifier) {
                            return std::string_view(n.as.ident.name, n.as.ident.length) == lt.name;
                        }
                        if (n.type == NodeType::TableAccess)
                            return refs_var(n.as.table_access.table) || refs_var(n.as.table_access.key);
                        if (n.type == NodeType::ParenExpression)
                            return refs_var(n.as.paren_expr.expr);
                        if (n.type == NodeType::UnaryOp)
                            return refs_var(n.as.unary_op.expr);
                        if (n.type == NodeType::BinaryOp)
                            return refs_var(n.as.bin_op.left) || refs_var(n.as.bin_op.right);
                        if (n.type == NodeType::CallExpression) {
                            if (refs_var(n.as.call_expr.target))
                                return true;
                            for (uint32_t ai = 0; ai < n.as.call_expr.arg_count; ++ai) {
                                if (refs_var(ctx.block_statements[n.as.call_expr.first_arg + ai]))
                                    return true;
                            }
                            return false;
                        }
                        if (n.type == NodeType::TableConstructor) {
                            for (uint32_t ei = 0; ei < n.as.table_cons.count; ++ei) {
                                if (refs_var(ctx.block_statements[n.as.table_cons.first_item + ei * 2])
                                    || refs_var(ctx.block_statements[n.as.table_cons.first_item + ei * 2 + 1]))
                                    return true;
                            }
                            return false;
                        }
                        if (n.type == NodeType::IntrinsicCall) {
                            for (uint32_t ai = 0; ai < n.as.intrinsic_call.arg_count; ++ai) {
                                if (refs_var(ctx.block_statements[n.as.intrinsic_call.first_arg + ai]))
                                    return true;
                            }
                            return false;
                        }
                        return false;
                    };
                    std::function<bool(uint32_t)> flows_ref = [&](uint32_t n_idx) -> bool {
                        if (n_idx >= ctx.nodes.size())
                            return false;
                        const auto &n = ctx.nodes[n_idx];
                        if (n.type == NodeType::Identifier)
                            return std::string_view(n.as.ident.name, n.as.ident.length) == lt.name;
                        if (n.type == NodeType::ParenExpression)
                            return flows_ref(n.as.paren_expr.expr);
                        if (n.type == NodeType::TableConstructor) {
                            for (uint32_t ei = 0; ei < n.as.table_cons.count; ++ei) {
                                if (flows_ref(ctx.block_statements[n.as.table_cons.first_item + ei * 2])
                                    || flows_ref(ctx.block_statements[n.as.table_cons.first_item + ei * 2 + 1]))
                                    return true;
                            }
                            return false;
                        }
                        if (n.type == NodeType::CallExpression) {
                            if (flows_ref(n.as.call_expr.target))
                                return true;
                            for (uint32_t ai = 0; ai < n.as.call_expr.arg_count; ++ai) {
                                if (flows_ref(ctx.block_statements[n.as.call_expr.first_arg + ai]))
                                    return true;
                            }
                            return false;
                        }
                        return false;
                    };

                    if (sn.type == NodeType::ReturnStatement) {
                        for (uint32_t ri = 0; ri < sn.as.return_stmt.value_count; ++ri) {
                            uint32_t v_idx = ctx.block_statements[sn.as.return_stmt.first_value + ri];
                            if (refs_var(v_idx)) {
                                escapes = true;
                                return;
                            }
                        }
                    }

                    if (sn.type == NodeType::Assignment || sn.type == NodeType::GlobalDeclStatement) {
                        uint32_t t_count = (sn.type == NodeType::GlobalDeclStatement) ? sn.as.global_decl.ident_count
                                                                                      : sn.as.assign.target_count;
                        uint32_t first_t = (sn.type == NodeType::GlobalDeclStatement) ? sn.as.global_decl.first_ident
                                                                                      : sn.as.assign.first_target;
                        for (uint32_t ti = 0; ti < t_count; ++ti) {
                            uint32_t tgt = ctx.block_statements[first_t + ti];
                            if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier
                                && ctx.nodes[tgt].as.ident.is_global) {
                                uint32_t v_count = (sn.type == NodeType::GlobalDeclStatement)
                                    ? sn.as.global_decl.value_count
                                    : sn.as.assign.value_count;
                                uint32_t first_v = (sn.type == NodeType::GlobalDeclStatement)
                                    ? sn.as.global_decl.first_value
                                    : sn.as.assign.first_value;
                                if (ti < v_count) {
                                    uint32_t v_idx = ctx.block_statements[first_v + ti];
                                    if (refs_var(v_idx)) {
                                        escapes = true;
                                        return;
                                    }
                                }
                            }
                        }
                    }

                    if (sn.type == NodeType::LocalDecl) {
                        for (uint32_t li = 0; li < sn.as.local_decl.ident_count; ++li) {
                            uint32_t id_idx = ctx.block_statements[sn.as.local_decl.first_ident + li];
                            if (id_idx < ctx.nodes.size() && ctx.nodes[id_idx].type == NodeType::Identifier) {
                                std::string_view tname(
                                    ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                                if (li < sn.as.local_decl.value_count) {
                                    uint32_t v_idx = ctx.block_statements[sn.as.local_decl.first_value + li];
                                    if (refs_var(v_idx) && state.escaping_vars.count(tname)) {
                                        escapes = true;
                                        return;
                                    }
                                }
                            }
                        }
                    }
                    if (sn.type == NodeType::Assignment) {
                        for (uint32_t ti = 0; ti < sn.as.assign.target_count; ++ti) {
                            uint32_t tgt = ctx.block_statements[sn.as.assign.first_target + ti];
                            if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier
                                && !ctx.nodes[tgt].as.ident.is_global) {
                                std::string_view tname(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                                if (ti < sn.as.assign.value_count) {
                                    uint32_t v_idx = ctx.block_statements[sn.as.assign.first_value + ti];
                                    if (refs_var(v_idx) && state.escaping_vars.count(tname)) {
                                        escapes = true;
                                        return;
                                    }
                                }
                            }
                        }
                    }

                    if (sn.type == NodeType::LocalDecl || sn.type == NodeType::Assignment) {
                        uint32_t v_count = (sn.type == NodeType::LocalDecl) ? sn.as.local_decl.value_count
                                                                            : sn.as.assign.value_count;
                        uint32_t first_v = (sn.type == NodeType::LocalDecl) ? sn.as.local_decl.first_value
                                                                            : sn.as.assign.first_value;
                        for (uint32_t vi = 0; vi < v_count; ++vi) {
                            uint32_t v_idx = ctx.block_statements[first_v + vi];
                            if (flows_ref(v_idx)) {
                                escapes = true;
                                return;
                            }
                        }
                        if (sn.type == NodeType::Assignment) {
                            for (uint32_t ti = 0; ti < sn.as.assign.target_count; ++ti) {
                                uint32_t tgt = ctx.block_statements[sn.as.assign.first_target + ti];
                                if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::TableAccess) {
                                    for (uint32_t vi = 0; vi < v_count; ++vi) {
                                        if (flows_ref(ctx.block_statements[first_v + vi])) {
                                            escapes = true;
                                            return;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (sn.type == NodeType::CallExpression) {
                        for (uint32_t ai = 0; ai < sn.as.call_expr.arg_count; ++ai) {
                            uint32_t a_idx = ctx.block_statements[sn.as.call_expr.first_arg + ai];
                            if (refs_var(a_idx)) {
                                escapes = true;
                                return;
                            }
                        }
                    }

                    if (sn.type == NodeType::Block) {
                        check_escape(stmt);
                        continue;
                    }
                    if (sn.type == NodeType::IfStatement) {
                        check_escape(sn.as.if_stmt.then_block);
                        check_escape(sn.as.if_stmt.else_block);
                        continue;
                    }
                    if (sn.type == NodeType::WhileStatement) {
                        check_escape(sn.as.while_stmt.body_block);
                        continue;
                    }
                    if (sn.type == NodeType::RepeatStatement) {
                        check_escape(sn.as.repeat_stmt.body_block);
                        continue;
                    }
                    if (sn.type == NodeType::ForStatement) {
                        check_escape(sn.as.for_stmt.body_block);
                        continue;
                    }
                    if (sn.type == NodeType::GenericForStatement) {
                        check_escape(sn.as.generic_for.body_block);
                        continue;
                    }
                    if (sn.type == NodeType::DoStatement) {
                        check_escape(sn.as.do_stmt.body_block);
                        continue;
                    }
                    if (sn.type == NodeType::FunctionDef) {
                        continue;
                    }
                }
            };
            check_escape(body);

            if (!escapes) {
                for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
                    const auto &n = ctx.nodes[ni];
                    if (n.type == NodeType::Identifier && n.as.ident.is_captured
                        && std::string_view(n.as.ident.name, n.as.ident.length) == lt.name) {
                        escapes = true;
                        break;
                    }
                }
            }

            if (!escapes) {
                bool might_grow = false;
                std::function<void(uint32_t)> check_growth = [&](uint32_t block_idx) {
                    if (might_grow || block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size())
                        return;
                    const auto &blk = ctx.nodes[block_idx];
                    if (blk.type != NodeType::Block)
                        return;
                    for (uint32_t si = 0; si < blk.as.block.count; ++si) {
                        if (might_grow)
                            return;
                        uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + si];
                        if (stmt >= ctx.nodes.size())
                            continue;
                        const auto &sn = ctx.nodes[stmt];
                        auto check_target = [&](uint32_t tgt_idx) {
                            if (tgt_idx >= ctx.nodes.size())
                                return;
                            const auto &tn = ctx.nodes[tgt_idx];
                            if (tn.type == NodeType::TableAccess) {
                                if (tn.as.table_access.table < ctx.nodes.size()
                                    && ctx.nodes[tn.as.table_access.table].type == NodeType::Identifier) {
                                    std::string_view tname(ctx.nodes[tn.as.table_access.table].as.ident.name,
                                        ctx.nodes[tn.as.table_access.table].as.ident.length);
                                    if (tname == lt.name)
                                        might_grow = true;
                                }
                            }
                        };
                        if (sn.type == NodeType::Assignment) {
                            for (uint32_t ti = 0; ti < sn.as.assign.target_count; ++ti)
                                check_target(ctx.block_statements[sn.as.assign.first_target + ti]);
                        }
                        if (sn.type == NodeType::Block)
                            check_growth(stmt);
                        else if (sn.type == NodeType::IfStatement) {
                            check_growth(sn.as.if_stmt.then_block);
                            check_growth(sn.as.if_stmt.else_block);
                        } else if (sn.type == NodeType::WhileStatement)
                            check_growth(sn.as.while_stmt.body_block);
                        else if (sn.type == NodeType::RepeatStatement)
                            check_growth(sn.as.repeat_stmt.body_block);
                        else if (sn.type == NodeType::ForStatement)
                            check_growth(sn.as.for_stmt.body_block);
                        else if (sn.type == NodeType::GenericForStatement)
                            check_growth(sn.as.generic_for.body_block);
                        else if (sn.type == NodeType::DoStatement)
                            check_growth(sn.as.do_stmt.body_block);
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

        uint32_t total_arena = 0;
        for (uint32_t node_idx : state.arena_safe_table_nodes) {
            const auto &tc = ctx.nodes[node_idx];
            if (tc.type != NodeType::TableConstructor)
                continue;
            size_t arr_count = 0;
            size_t hash_count = 0;
            for (uint32_t ei = 0; ei < tc.as.table_cons.count; ++ei) {
                uint32_t k_idx = ctx.block_statements[tc.as.table_cons.first_item + ei * 2];
                if (k_idx == 0xFFFFFFFF) {
                    arr_count++;
                } else {
                    hash_count++;
                }
            }
            if (state.table_presize.count(node_idx)) {
                arr_count = 16;
            }
            constexpr size_t HDR = sizeof(clx::LTable);
            constexpr size_t TVAL = sizeof(clx::TValue);
            constexpr size_t VT = sizeof(clx::ValueType);
            constexpr size_t HENTRY = sizeof(clx::HashEntry);
            constexpr size_t EXT = sizeof(clx::LTableExt);
            if (arr_count < CLX_ARENA_DEFAULT_FIELDS)
                arr_count = CLX_ARENA_DEFAULT_FIELDS;
            if (hash_count < CLX_ARENA_DEFAULT_FIELDS)
                hash_count = CLX_ARENA_DEFAULT_FIELDS;
            size_t aligned_arr = ((TVAL * arr_count + 7) & ~static_cast<size_t>(7));
            size_t aligned_types = ((VT * arr_count + 7) & ~static_cast<size_t>(7));
            size_t aligned_hash = ((HENTRY * hash_count + 7) & ~static_cast<size_t>(7));
            total_arena += static_cast<uint32_t>(HDR + aligned_arr + aligned_types + aligned_hash + EXT);
        }
        if (total_arena > 0) {
            state.arena_table_sizes[fi] = total_arena;
        }
    }
}

}
