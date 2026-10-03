// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  assignments.cpp · Declarations & assigns   │
// └─────────────────────────────────────────────┘

#ifdef _WIN32
#define NOMINMAX
#endif
#include "codegen.h"
#include "helpers.h"
#include "../../include/clx.h"
#include "../optimizer/optimizer.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clx {

//------------------ CodeEmitter::hoisted_table_ptr (see codegen.h)
std::string CodeEmitter::hoisted_table_ptr(uint32_t table_idx) {
    if (table_idx >= ctx.nodes.size() || ctx.nodes[table_idx].type != NodeType::Identifier
        || ctx.nodes[table_idx].as.ident.is_global)
        return "";
    std::string_view nm(ctx.nodes[table_idx].as.ident.name, ctx.nodes[table_idx].as.ident.length);
    auto it = state.hoisted_tables.find(nm);
    return it == state.hoisted_tables.end() ? std::string() : it->second;
}

//------------------ CodeEmitter::local_root_snapshot_ok: cached per-local verdict on snapshot-slot rooting (see scan_own).
bool CodeEmitter::local_root_snapshot_ok(std::string_view lua_name) {
    auto key = std::make_pair(state.current_func_body, lua_name);
    auto it = state.register_friendly_locals.find(key);
    if (it != state.register_friendly_locals.end())
        return it->second;
    bool ok = local_register_friendly(ctx, state, state.current_func_body, lua_name);
    state.register_friendly_locals[key] = ok;
    return ok;
}

//------------------ CodeEmitter::emit_local_root (see codegen.h)
void CodeEmitter::emit_local_root(std::string_view name) {
    if (state.reassigned_vars.count(name) == 0 && local_root_snapshot_ok(name)) {
        out << "clx::LValue _rs_" << name << " = l_" << name << ";\n";
        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_rs_" << name << ".val, &_rs_" << name
            << ".type);\n";
        return;
    }
    out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_" << name << ".type);\n";
}

//------------------ CodeEmitter::emit_param_root (see codegen.h)
void CodeEmitter::emit_param_root(std::string_view lua_name, std::string_view cpp_name) {
    if (state.assigned_targets.count(lua_name) == 0 && local_root_snapshot_ok(lua_name)) {
        out << "clx::LValue _rs_" << cpp_name << " = l_" << cpp_name << ";\n";
        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_rs_" << cpp_name << ".val, &_rs_" << cpp_name
            << ".type);\n";
        return;
    }
    out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << cpp_name << ".val, &l_" << cpp_name
        << ".type);\n";
}

//------------------ emitAssignmentLike: dispatches GlobalDeclStatement, LocalDecl, and Assignment.
void CodeEmitter::emitAssignmentLike(const ASTNode &node, uint32_t node_idx) {
    if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard)
        return;
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";

    bool is_local = (node.type == NodeType::LocalDecl);
    bool is_global = (node.type == NodeType::GlobalDeclStatement);
    size_t csnap_base = state.const_snapshot_cells.size();
    uint32_t t_count = is_local ? node.as.local_decl.ident_count
                                : (is_global ? node.as.global_decl.ident_count : node.as.assign.target_count);
    uint32_t v_count = is_local ? node.as.local_decl.value_count
                                : (is_global ? node.as.global_decl.value_count : node.as.assign.value_count);
    uint32_t first_t = is_local ? node.as.local_decl.first_ident
                                : (is_global ? node.as.global_decl.first_ident : node.as.assign.first_target);
    uint32_t first_v = is_local ? node.as.local_decl.first_value
                                : (is_global ? node.as.global_decl.first_value : node.as.assign.first_value);

    bool last_is_call = false;
    if (v_count > 0) {
        uint32_t last_v_idx = ctx.block_statements[first_v + v_count - 1];
        if (ctx.nodes[last_v_idx].type == NodeType::CallExpression || ctx.nodes[last_v_idx].type == NodeType::Vararg)
            last_is_call = true;
    }

    bool is_single_native = false;
    std::string_view single_name;
    if (t_count == 1 && !last_is_call) {
        uint32_t t_idx = ctx.block_statements[first_t];
        if (ctx.nodes[t_idx].type == NodeType::Identifier && !ctx.nodes[t_idx].as.ident.is_global) {
            single_name = std::string_view(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
            bool in_native = std::find(state.native_numbers.begin(), state.native_numbers.end(), single_name)
                != state.native_numbers.end();
            bool rhs_yields_number = (v_count > 0)
                ? yields_number(ctx, state, ctx.block_statements[first_v], nullptr, state.current_fast_func)
                : false;
            bool rhs_self_compound = false;
            if (!is_local && in_native && !rhs_yields_number && v_count > 0) {
                uint32_t rv = ctx.block_statements[first_v];
                if (ctx.nodes[rv].type == NodeType::BinaryOp) {
                    int bop0 = ctx.nodes[rv].as.bin_op.op;
                    if (bop0 == static_cast<int>(BinaryOp::Add) || bop0 == static_cast<int>(BinaryOp::Sub)) {
                        auto is_tgt0 = [&](uint32_t e) {
                            return ctx.nodes[e].type == NodeType::Identifier
                                && std::string_view(ctx.nodes[e].as.ident.name, ctx.nodes[e].as.ident.length)
                                == single_name;
                        };
                        rhs_self_compound
                            = is_tgt0(ctx.nodes[rv].as.bin_op.left) || is_tgt0(ctx.nodes[rv].as.bin_op.right);
                    }
                }
            }
            if (!ctx.nodes[t_idx].as.ident.is_captured) {
                if (is_local) {
                    if (in_native || rhs_yields_number)
                        is_single_native = true;
                } else {
                    if (in_native && (rhs_yields_number || rhs_self_compound))
                        is_single_native = true;
                }
            }
        }
    }

    if (is_single_native) {

        if (std::find(state.native_numbers.begin(), state.native_numbers.end(), single_name)
            == state.native_numbers.end())
            state.native_numbers.push_back(single_name);
        if (is_local) {
            bool is_int_single = state.native_integers.count(single_name) > 0;
            if (!is_int_single && v_count > 0) {
                uint32_t v_idx_single = ctx.block_statements[first_v];
                bool redecl_of_native = false;
                for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                    if (it->name == single_name) {
                        redecl_of_native = true;
                        break;
                    }
                }
                if (clx::is_purely_integer_expr(ctx, state, v_idx_single)
                    && !var_reassigned_non_int(single_name, state.current_func_body, v_idx_single)
                    && !redecl_of_native) {
                    is_int_single = true;
                    state.native_integers.insert(std::string(single_name));
                }
            }
            bool is_shadow_decl = false;
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                if (it->name == single_name) {
                    is_shadow_decl = true;
                    break;
                }
            }
            bool use_rhs_tmp = is_shadow_decl && (v_count > 0)
                && yields_number(ctx, state, ctx.block_statements[first_v], nullptr, state.current_fast_func);
            std::string rhs_flag_text;
            if (v_count > 0)
                rhs_flag_text = int_flag_expr(ctx.block_statements[first_v], 1);
            if (use_rhs_tmp) {
                out << "double _rhs_" << node_idx << " = ";
                emit_native(ctx.block_statements[first_v]);
                out << ";\n";
                out << "bool _rhsf_" << node_idx << " = (" << rhs_flag_text << ");\n";
                out << "int64_t _rhsi_" << node_idx << " = (" << int_value_expr(ctx.block_statements[first_v], 1)
                    << ");\n";
            }
            if (!state.hoisted_locals.count(single_name)) {
                if (is_int_single) {
                    out << "int64_t l_" << single_name << ";\n";
                } else {
                    out << "double l_" << single_name << ";\n";
                    out << "bool _intf_l_" << single_name << " = false;\n";
                    out << "int64_t _ii_l_" << single_name << " = 0;\n";
                }
            }
            out << "l_" << single_name << " = ";
            if (use_rhs_tmp) {
                out << "_rhs_" << node_idx;
            } else if (v_count > 0) {
                uint32_t v_idx = ctx.block_statements[first_v];
                emit_native(v_idx);
            } else
                out << "0.0";
            out << ";\n";
            locals.push_back({ single_name, false });

            if (!is_int_single) {
                locals.back().has_intf = true;
                locals.back().has_ii = true;
                if (v_count > 0) {
                    if (use_rhs_tmp) {
                        out << "_intf_l_" << single_name << " = _rhsf_" << node_idx << ";\n";
                        out << "_ii_l_" << single_name << " = _rhsi_" << node_idx << ";\n";
                    } else {
                        out << "_intf_l_" << single_name << " = (" << rhs_flag_text << ");\n";
                        out << "_ii_l_" << single_name << " = (" << int_value_expr(ctx.block_statements[first_v], 1)
                            << ");\n";
                    }
                } else {
                    out << "_intf_l_" << single_name << " = false;\n";
                    out << "_ii_l_" << single_name << " = 0;\n";
                }
            } else {
                locals.back().has_intf = false;
            }
        } else {
            bool is_boxed = false;
            bool is_loc = this->is_local(single_name, is_boxed);
            if (is_boxed) {
                out << "clx::upval_store(l_" << single_name << ", clx::LValue(static_cast<double>(";
                if (v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    emit_native(v_idx);
                } else
                    out << "0.0";
                out << ")));";
            } else {
                bool handled_compound = false;
                if (is_loc && v_count > 0) {
                    uint32_t v_idx0 = ctx.block_statements[first_v];
                    if (ctx.nodes[v_idx0].type == NodeType::BinaryOp) {
                        int bop = ctx.nodes[v_idx0].as.bin_op.op;
                        if (bop == static_cast<int>(BinaryOp::Add) || bop == static_cast<int>(BinaryOp::Sub)) {
                            uint32_t lft = ctx.nodes[v_idx0].as.bin_op.left;
                            uint32_t rgt = ctx.nodes[v_idx0].as.bin_op.right;
                            auto is_self = [&](uint32_t e) {
                                return ctx.nodes[e].type == NodeType::Identifier
                                    && std::string_view(ctx.nodes[e].as.ident.name, ctx.nodes[e].as.ident.length)
                                    == single_name
                                    && yields_number(ctx, state, e, nullptr, state.current_fast_func);
                            };
                            bool l_self = is_self(lft);
                            bool r_self = is_self(rgt);
                            bool l_native = yields_number(ctx, state, lft, nullptr, state.current_fast_func);
                            bool r_native = yields_number(ctx, state, rgt, nullptr, state.current_fast_func);
                            if ((l_self && !r_native) || (r_self && !l_native)) {
                                for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                                    if (it->name == single_name && it->has_intf) {
                                        uint32_t other = l_self ? rgt : lft;
                                        bool use_ii = it->has_ii;
                                        out << "{ clx::LValue _rv" << node_idx << " = ";
                                        emit_node(other);
                                        out << "; l_" << single_name << " = (l_" << single_name
                                            << (bop == static_cast<int>(BinaryOp::Add) ? " + " : " - ") << "_rv"
                                            << node_idx << ".as_number()); _intf_l_" << single_name << " = _intf_l_"
                                            << single_name << " && _rv" << node_idx
                                            << ".type == clx::ValueType::Int64;";
                                        if (use_ii) {
                                            out << " _ii_l_" << single_name << " = _intf_l_" << single_name
                                                << " ? clx::int_"
                                                << (bop == static_cast<int>(BinaryOp::Add) ? "add" : "sub") << "(_ii_l_"
                                                << single_name << ", _rv" << node_idx << ".val.payload.i64) : 0;";
                                        }
                                        out << " }";
                                        handled_compound = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                if (!handled_compound) {
                    out << "l_" << single_name << " = ";
                    if (v_count > 0) {
                        uint32_t v_idx = ctx.block_statements[first_v];
                        emit_native(v_idx);
                    } else
                        out << "0.0";
                    if (is_loc && v_count > 0) {
                        for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                            if (it->name == single_name) {
                                if (it->has_intf) {
                                    out << ";\n_intf_l_" << single_name << " = ("
                                        << int_flag_expr(ctx.block_statements[first_v], 1) << ")";
                                    if (it->has_ii)
                                        out << ";\n_ii_l_" << single_name << " = ("
                                            << int_value_expr(ctx.block_statements[first_v], 1) << ")";
                                }
                                break;
                            }
                        }
                    }
                }
            }
            out << ";\n";
        }
        return;
    }

    bool is_single_dynamic = (t_count == 1 && v_count <= 1 && !last_is_call && !is_single_native);
    if (is_single_dynamic) {
        uint32_t t_idx = ctx.block_statements[first_t];
        const auto &t_node = ctx.nodes[t_idx];

        if (t_node.type == NodeType::Identifier) {
            std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
            bool is_boxed = false;
            bool is_loc = this->is_local(name, is_boxed);

            if (is_local) {
                bool intercepted = false;
                if (v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    if (ctx.nodes[v_idx].type == NodeType::FunctionDef && !t_node.as.ident.is_global) {
                        if (state.reassigned_vars.count(name) == 0) {
                            bool is_fast = false;
                            if (state.native_return_funcs.count(name) && state.func_param_native.count(name)) {
                                is_fast = true;
                                for (bool p : state.func_param_native[name])
                                    if (!p)
                                        is_fast = false;
                            }
                            if (!is_fast && state.native_direct_funcs.count(name)) {
                                emit_native_impl_def(name, v_idx);
                            }

                            if (is_fast) {
                                out << "auto _fast_" << name << "_impl = ";
                                state.emit_fast_lambda = true;
                                state.in_fast_function = true;
                                state.current_fast_func = name;
                                emit_node(v_idx);
                                state.emit_fast_lambda = false;
                                state.in_fast_function = false;
                                state.current_fast_func = "";
                                out << ";\n";
                                out << "#line " << ctx.nodes[v_idx].line << " \"" << ctx.filename << "\"\n";
                                out << "auto _fast_" << name << " = [=]( ";
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << "double p" << a << (a < state.func_param_counts[name] - 1 ? ", " : "");
                                }
                                out << ") -> double { return _fast_" << name << "_impl(_fast_" << name << "_impl";
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << ", p" << a;
                                }
                                out << "); };\n";

                                out << "auto _impl_" << name
                                    << " = [=](clx::LState* L, const clx::LValue* args, size_t arg_count) -> "
                                       "clx::MultiValue {\n";
                                if (state.int_returning_funcs.count(name)) {
                                    out << "    return clx::MultiValue(clx::LValue(static_cast<int64_t>(_fast_" << name
                                        << "(";
                                } else if (state.int_preserving_masks.count(name)) {
                                    out << "    bool _bflag = ";
                                    uint32_t _m = state.int_preserving_masks.at(name);
                                    bool _any_f = false;
                                    for (uint32_t p = 0; p < state.func_param_counts[name] && p < 32; ++p) {
                                        if (!(_m & (1u << p)))
                                            continue;
                                        out << (_any_f ? " && " : "") << "(" << p << " < arg_count) && args[" << p
                                            << "].type == clx::ValueType::Int64";
                                        _any_f = true;
                                    }
                                    if (!_any_f)
                                        out << "false";
                                    out << ";\n";
                                    out << "    return clx::MultiValue(clx::num_box(_fast_" << name << "(";
                                } else {
                                    out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << name
                                        << "(";
                                }
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                    if (a < state.func_param_counts[name] - 1)
                                        out << ", ";
                                }
                                if (state.int_returning_funcs.count(name))
                                    out << "))));\n";
                                else if (state.int_preserving_masks.count(name))
                                    out << "), _bflag));\n";
                                else
                                    out << "))));\n";
                                out << "};\n";
                                std::string cells_b = gc_cells_arg();
                                out << "#line " << ctx.nodes[v_idx].line << " \"" << ctx.filename << "\"\n";
                                out << "clx::LValue l_" << name << " = L->create_closure(_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer())"
                                    << (cells_b.empty() ? "" : ", " + cells_b) << ");\n";
                                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_"
                                    << name << ".type);\n";
                                {
                                    size_t _sfi = state.string_pool_index.at(name);
                                    out << "L->_G->settable(cstr_[" << _sfi << "], l_" << name << ");\n";
                                }
                                locals.push_back({ name, false });
                                state.fast_callables.insert(name);
                            } else {
                                out << "clx::LValue l_" << name << ";\n";
                                out << "auto l_" << name << "_cell = std::make_shared<clx::LValue>();\n";
                                out << "auto _impl_" << name
                                    << " = std::make_shared<std::function<clx::MultiValue(clx::LState*, const "
                                       "clx::LValue*, size_t)>>();\n";
                                std::string saved_raw_cell = state.raw_lambda_cell;
                                state.raw_lambda_cell = std::string(name);
                                if (state.string_builders.count(name) && !state.module_string_builders.count(name)) {
                                    state.ref_capture = "sb_" + std::string(name);
                                }
                                state.holder_cell_callables.insert(name);
                                locals.push_back({ name, false });
                                state.direct_callables.insert(name);
                                out << "*_impl_" << name << " = ";
                                state.emit_raw_lambda = true;
                                emit_node(v_idx);
                                state.emit_raw_lambda = false;
                                state.raw_lambda_cell = saved_raw_cell;
                                state.ref_capture.clear();
                                out << ";\n";
                                std::string cells_c = gc_cells_arg(std::string(name));
                                out << "#line " << ctx.nodes[v_idx].line << " \"" << ctx.filename << "\"\n";
                                out << "l_" << name << " = L->create_closure(*_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer())"
                                    << (cells_c.empty() ? "" : ", " + cells_c) << ");\n";
                                out << "clx::upval_store(l_" << name << "_cell, l_" << name << ");\n";
                                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_"
                                    << name << ".type);\n";
                                {
                                    size_t _sfi = state.string_pool_index.at(name);
                                    out << "L->_G->settable(cstr_[" << _sfi << "], l_" << name << ");\n";
                                }
                            }

                            intercepted = true;
                        }
                    }
                }

                if (!intercepted) {
                    if (v_count > 0 && ctx.nodes[ctx.block_statements[first_v]].type == NodeType::TableConstructor
                        && state.pure_numeric_arrays.count(name)) {
                        uint32_t v_idx = ctx.block_statements[first_v];
                        const auto &tc = ctx.nodes[v_idx].as.table_cons;
                        bool int_array = state.int_numeric_arrays.count(std::string(name)) > 0;
                        if (state.table_presize.count(v_idx)) {
                            out << (int_array ? "std::vector<int64_t> l_" : "std::vector<double> l_") << name
                                << "(static_cast<size_t>(";
                            emit_native(state.table_presize[v_idx]);
                            out << (int_array ? "), 0);\n" : "), 0.0);\n");
                        } else {
                            out << (int_array ? "std::vector<int64_t> l_" : "std::vector<double> l_") << name << ";\n";
                            out << "l_" << name << ".reserve(" << tc.count << ");\n";
                        }
                        for (uint32_t ei = 0; ei < tc.count; ++ei) {
                            uint32_t ev = ctx.block_statements[tc.first_item + ei * 2 + 1];
                            out << "l_" << name << ".push_back(";
                            emit_native(ev);
                            out << ");\n";
                        }
                        locals.push_back({ name, false });
                    } else if (t_node.as.ident.is_captured) {
                        bool is_const = state.constant_upvalues.count(name) > 0;
                        if (is_const) {
                            {
                                uint32_t _alias_v_idx3 = ctx.block_statements[first_v];
                                const auto &_alias_v_node3 = ctx.nodes[_alias_v_idx3];
                                if (v_count > 0 && !t_node.as.ident.is_global
                                    && _alias_v_node3.type == NodeType::TableAccess
                                    && state.reassigned_vars.count(name) == 0) {
                                    uint32_t _ht4 = _alias_v_node3.as.table_access.table;
                                    uint32_t _hk4 = _alias_v_node3.as.table_access.key;
                                    if (_ht4 < ctx.nodes.size() && _hk4 < ctx.nodes.size()
                                        && ctx.nodes[_ht4].type == NodeType::Identifier
                                        && ctx.nodes[_hk4].type == NodeType::String) {
                                        std::string_view _hm4(
                                            ctx.nodes[_ht4].as.ident.name, ctx.nodes[_ht4].as.ident.length);
                                        std::string_view _hf4(
                                            ctx.nodes[_hk4].as.string.text, ctx.nodes[_hk4].as.string.length);
                                        const char *_cf4 = lookup_builtin(_hm4, _hf4);
                                        if (_cf4)
                                            state.builtin_aliases[std::string(name)] = _cf4;
                                    }
                                }
                            }
                            if (state.hoisted_locals.count(name)) {
                                if (v_count > 0) {
                                    out << "l_" << name << " = ";
                                    emit_node(ctx.block_statements[first_v]);
                                    out << ";\n";
                                }
                            } else {
                                out << "clx::LValue l_" << name << " = ";
                                if (v_count > 0)
                                    emit_node(ctx.block_statements[first_v]);
                                else
                                    out << "clx::LValue()";
                                out << ";\nL->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_"
                                    << name << ".type);\n";
                            }
                            bool created_csnap = false;
                            if (!state.pure_numeric_arrays.count(name)) {
                                out << "auto l_" << name << "_csnap = clx::make_upvalue(l_" << name << ");\n";
                                state.const_snapshot_cells.push_back(std::string(name));
                                created_csnap = true;
                            }
                            if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                                && !state.module_string_builders.count(name)) {
                                out << "clx::StringBuilder sb_" << name << ";\n";
                                locals.back().has_sb = true;
                            }
                            locals.push_back({ name, false });
                            locals.back().has_csnap = created_csnap;
                            if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                                && !state.module_string_builders.count(name)) {
                                locals.back().has_sb = true;
                            }

                            if (t_node.as.ident.attr == clx::Attribute::Close) {
                                out << "clx::CloseGuard __cg_" << node_idx << "_0(L, l_" << name << ");\n";
                            }
                        } else {
                            bool is_func
                                = v_count > 0 && ctx.nodes[ctx.block_statements[first_v]].type == NodeType::FunctionDef;
                            if (!state.hoisted_locals.count(name)) {
                                out << "clx::LUpValue l_" << name << ";\n";
                            }
                            if (is_func) {
                                out << "l_" << name << " = clx::make_upvalue(clx::LValue());\n";
                                if (state.string_builders.count(name) && !state.module_string_builders.count(name)) {
                                    state.ref_capture = "sb_" + std::string(name);
                                }
                            }
                            locals.push_back({ name, true });
                            if (is_func) {
                                out << "clx::upval_store(l_" << name << ", ";
                                emit_node(ctx.block_statements[first_v]);
                                out << ");\n";
                            } else {
                                out << "l_" << name << " = clx::make_upvalue(";
                                if (v_count > 0)
                                    emit_node(ctx.block_statements[first_v]);
                                else
                                    out << "clx::LValue()";
                                out << ");\n";
                            }
                            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << "->val, &l_"
                                << name << "->type);\n";
                            if (is_func) {
                                state.ref_capture.clear();
                                size_t _sfi = state.string_pool_index.at(name);
                                out << "L->_G->settable(cstr_[" << _sfi << "], *l_" << name << ");\n";
                            }
                            if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                                && !state.module_string_builders.count(name)) {
                                out << "clx::StringBuilder sb_" << name << ";\n";
                                locals.back().has_sb = true;
                            }
                            if (t_node.as.ident.attr == clx::Attribute::Close) {
                                out << "clx::CloseGuard __cg_" << node_idx << "_0(L, *l_" << name << ");\n";
                            }
                        }
                    } else {
                        {
                            uint32_t _alias_v_idx = ctx.block_statements[first_v];
                            const auto &_alias_v_node = ctx.nodes[_alias_v_idx];
                            if (v_count > 0 && !t_node.as.ident.is_global && !t_node.as.ident.is_captured
                                && _alias_v_node.type == NodeType::TableAccess
                                && state.reassigned_vars.count(name) == 0) {
                                uint32_t _ht2 = _alias_v_node.as.table_access.table;
                                uint32_t _hk2 = _alias_v_node.as.table_access.key;
                                if (_ht2 < ctx.nodes.size() && _hk2 < ctx.nodes.size()
                                    && ctx.nodes[_ht2].type == NodeType::Identifier
                                    && ctx.nodes[_hk2].type == NodeType::String) {
                                    std::string_view _hm2(
                                        ctx.nodes[_ht2].as.ident.name, ctx.nodes[_ht2].as.ident.length);
                                    std::string_view _hf2(
                                        ctx.nodes[_hk2].as.string.text, ctx.nodes[_hk2].as.string.length);
                                    const char *_cf2 = lookup_builtin(_hm2, _hf2);
                                    if (_cf2)
                                        state.builtin_aliases[std::string(name)] = _cf2;
                                }
                            }
                        }
                        if (state.hoisted_locals.count(name)) {
                            if (v_count > 0) {
                                out << "l_" << name << " = ";
                                emit_node(ctx.block_statements[first_v]);
                                out << ";\n";
                                bool hv_native
                                    = std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                                    != state.native_numbers.end();
                                if (hv_native) {
                                    out << "_intf_l_" << name << " = ("
                                        << int_flag_expr(ctx.block_statements[first_v], 1) << ");\n";
                                    out << "_ii_l_" << name << " = ("
                                        << int_value_expr(ctx.block_statements[first_v], 1) << ");\n";
                                }
                            }
                        } else {
                            out << "clx::LValue l_" << name << " = ";
                            if (v_count > 0)
                                emit_node(ctx.block_statements[first_v]);
                            else
                                out << "clx::LValue()";
                            out << ";\n";
                            emit_local_root(name);
                        }
                        if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                            && !state.module_string_builders.count(name)) {
                            out << "clx::StringBuilder sb_" << name << ";\n";
                        }
                        locals.push_back({ name, false });
                        if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                            && !state.module_string_builders.count(name)) {
                            locals.back().has_sb = true;
                        }

                        if (t_node.as.ident.attr == clx::Attribute::Close) {
                            out << "clx::CloseGuard __cg_" << node_idx << "_0(L, l_" << name << ");\n";
                        }
                    }
                }
            } else if (is_global && v_count == 0) {
            } else if (t_node.as.ident.is_global) {
                if (name == "_ENV") {
                    out << "_ENV = ";
                    if (v_count > 0)
                        emit_node(ctx.block_statements[first_v]);
                    else
                        out << "clx::LValue()";
                    out << ";\n";
                } else {
                    state.global_constants.erase(name);
                    auto it = std::find(state.native_numbers.begin(), state.native_numbers.end(), name);
                    if (it != state.native_numbers.end())
                        state.native_numbers.erase(it);

                    bool is_sb_concat = false;
                    std::vector<uint32_t> ops;
                    if (v_count > 0 && state.string_builders.count(name)) {
                        uint32_t v_idx = ctx.block_statements[first_v];
                        const auto &v_node = ctx.nodes[v_idx];
                        if (v_node.type == NodeType::BinaryOp
                            && v_node.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                            std::vector<uint32_t> ws;
                            ws.push_back(v_idx);
                            while (!ws.empty()) {
                                uint32_t cur = ws.back();
                                ws.pop_back();
                                const auto &cn = ctx.nodes[cur];
                                if (cn.type == NodeType::BinaryOp
                                    && cn.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                                    ws.push_back(cn.as.bin_op.right);
                                    ws.push_back(cn.as.bin_op.left);
                                } else {
                                    ops.push_back(cur);
                                }
                            }
                            if (!ops.empty() && ctx.nodes[ops[0]].type == NodeType::Identifier) {
                                std::string_view fn(ctx.nodes[ops[0]].as.ident.name, ctx.nodes[ops[0]].as.ident.length);
                                if (fn == name)
                                    is_sb_concat = true;
                            }
                        }
                    }

                    if (is_sb_concat) {

                        out << "{ clx::LValue _gval = clx::get_env_var(L, _ENV, \"" << name << "\");\n";
                        out << "  if (sb_" << name << ".empty()) sb_" << name << ".append(L, _gval);\n";
                        for (size_t i = 1; i < ops.size(); ++i) {
                            uint32_t op_idx = ops[i];
                            const auto &op_node = ctx.nodes[op_idx];
                            if (op_node.type == NodeType::String) {
                                size_t sfi = state.string_pool_index.at(
                                    std::string_view(op_node.as.string.text, op_node.as.string.length));
                                out << "  sb_" << name << ".append(cstr_[" << sfi << "].as_string(), cstr_[" << sfi
                                    << "].string_len());\n";
                            } else if (op_node.type == NodeType::Integer) {
                                out << "  sb_" << name << ".append(L, clx::integer(static_cast<int64_t>("
                                    << op_node.as.integer.val << ")));\n";
                            } else if (op_node.type == NodeType::Number) {
                                out << "  sb_" << name << ".append(L, clx::LValue(static_cast<double>("
                                    << op_node.as.number.val << ")));\n";
                            } else {
                                out << "  sb_" << name << ".append(L, ";
                                emit_node(op_idx);
                                out << ");\n";
                            }
                        }
                        out << "  clx::set_env_var(L, _ENV, \"" << name << "\", clx::LValue(sb_" << name
                            << ".to_string(L)));\n";
                        out << "}\n";
                    } else {
                        if (state.global_string_builders.count(name) || state.module_string_builders.count(name)) {
                            out << "    sb_" << name << ".clear();\n";
                        }
                        out << "clx::set_env_var(L, _ENV, \"" << name << "\", ";
                        if (v_count > 0)
                            emit_node(ctx.block_statements[first_v]);
                        else
                            out << "clx::LValue()";
                        out << ");\n";
                    }
                }
            } else if (is_loc) {
                bool intercepted = false;
                if (v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    if (ctx.nodes[v_idx].type == NodeType::FunctionDef) {
                        if (state.reassigned_vars.count(name) == 0) {
                            bool is_fast = false;
                            if (state.native_return_funcs.count(name) && state.func_param_native.count(name)) {
                                is_fast = true;
                                for (bool p : state.func_param_native[name])
                                    if (!p)
                                        is_fast = false;
                            }
                            if (!is_fast && state.native_direct_funcs.count(name)) {
                                emit_native_impl_def(name, v_idx);
                            }

                            if (is_fast) {
                                out << "auto _fast_" << name << "_impl = ";
                                state.emit_fast_lambda = true;
                                state.in_fast_function = true;
                                state.current_fast_func = name;
                                emit_node(v_idx);
                                state.emit_fast_lambda = false;
                                state.in_fast_function = false;
                                state.current_fast_func = "";
                                out << ";\n";
                                out << "#line " << ctx.nodes[v_idx].line << " \"" << ctx.filename << "\"\n";
                                out << "auto _fast_" << name << " = [=]( ";
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << "double p" << a << (a < state.func_param_counts[name] - 1 ? ", " : "");
                                }
                                out << ") -> double { return _fast_" << name << "_impl(_fast_" << name << "_impl";
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << ", p" << a;
                                }
                                out << "); };\n";

                                out << "auto _impl_" << name
                                    << " = [=](clx::LState* L, const clx::LValue* args, size_t arg_count) -> "
                                       "clx::MultiValue {\n";
                                if (state.int_returning_funcs.count(name)) {
                                    out << "    return clx::MultiValue(clx::LValue(static_cast<int64_t>(_fast_" << name
                                        << "(";
                                } else if (state.int_preserving_masks.count(name)) {
                                    out << "    bool _bflag = ";
                                    uint32_t _m = state.int_preserving_masks.at(name);
                                    bool _any_f = false;
                                    for (uint32_t p = 0; p < state.func_param_counts[name] && p < 32; ++p) {
                                        if (!(_m & (1u << p)))
                                            continue;
                                        out << (_any_f ? " && " : "") << "(" << p << " < arg_count) && args[" << p
                                            << "].type == clx::ValueType::Int64";
                                        _any_f = true;
                                    }
                                    if (!_any_f)
                                        out << "false";
                                    out << ";\n";
                                    out << "    return clx::MultiValue(clx::num_box(_fast_" << name << "(";
                                } else {
                                    out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << name
                                        << "(";
                                }
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                    if (a < state.func_param_counts[name] - 1)
                                        out << ", ";
                                }
                                if (state.int_returning_funcs.count(name))
                                    out << "))));\n";
                                else if (state.int_preserving_masks.count(name))
                                    out << "), _bflag));\n";
                                else
                                    out << "))));\n";
                                out << "};\n";
                                state.fast_callables.insert(name);
                            } else {
                                out << "auto _impl_" << name << " = ";
                                state.emit_raw_lambda = true;
                                emit_node(v_idx);
                                state.emit_raw_lambda = false;
                                out << ";\n";
                            }

                            state.direct_callables.insert(name);
                            std::string _gc_cells_d = gc_cells_arg(std::string(name));
                            if (is_boxed)
                                out << "clx::upval_store(l_" << name << ", L->create_closure(_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer())"
                                    << (_gc_cells_d.empty() ? "" : ", " + _gc_cells_d) << "));\n";
                            else
                                out << "#line " << ctx.nodes[v_idx].line << " \"" << ctx.filename << "\"\n";
                            out << "l_" << name << " = L->create_closure(_impl_" << name
                                << ", static_cast<clx::LTable*>(_ENV.as_pointer())"
                                << (_gc_cells_d.empty() ? "" : ", " + _gc_cells_d) << ");\n";
                            {
                                size_t _sfi2 = state.string_pool_index.at(name);
                                out << "L->_G->settable(cstr_[" << _sfi2 << "], "
                                    << (is_boxed ? "(*l_" + std::string(name) + ")" : "l_" + std::string(name))
                                    << ");\n";
                            }
                            intercepted = true;
                        }
                    }
                }
                if (!intercepted && v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    const auto &v_node = ctx.nodes[v_idx];

                    if (v_node.type == NodeType::BinaryOp
                        && v_node.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {

                        std::vector<uint32_t> concat_ops;
                        std::vector<uint32_t> walk_stack;
                        walk_stack.push_back(v_idx);
                        while (!walk_stack.empty()) {
                            uint32_t cur = walk_stack.back();
                            walk_stack.pop_back();
                            const auto &cn = ctx.nodes[cur];
                            if (cn.type == NodeType::BinaryOp
                                && cn.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                                walk_stack.push_back(cn.as.bin_op.right);
                                walk_stack.push_back(cn.as.bin_op.left);
                            } else {
                                concat_ops.push_back(cur);
                            }
                        }

                        if (!concat_ops.empty() && ctx.nodes[concat_ops[0]].type == NodeType::Identifier) {
                            std::string_view first_name(
                                ctx.nodes[concat_ops[0]].as.ident.name, ctx.nodes[concat_ops[0]].as.ident.length);
                            if (first_name == name && !ctx.nodes[concat_ops[0]].as.ident.is_global
                                && state.string_builders.count(name)) {

                                if (is_boxed) {
                                    out << "if (sb_" << name << ".empty()) sb_" << name << ".append(L, (*l_" << name
                                        << "));\n";
                                } else {
                                    out << "if (sb_" << name << ".empty()) sb_" << name << ".append(L, l_" << name
                                        << ");\n";
                                }

                                for (size_t i = 1; i < concat_ops.size(); ++i) {
                                    uint32_t op_idx = concat_ops[i];
                                    const auto &op_node = ctx.nodes[op_idx];
                                    if (op_node.type == NodeType::String) {
                                        size_t sfi = state.string_pool_index.at(
                                            std::string_view(op_node.as.string.text, op_node.as.string.length));
                                        out << "  sb_" << name << ".append(cstr_[" << sfi << "].as_string(), cstr_["
                                            << sfi << "].string_len());\n";
                                    } else if (op_node.type == NodeType::Integer) {
                                        out << "sb_" << name << ".append(L, clx::integer(static_cast<int64_t>("
                                            << op_node.as.integer.val << ")));\n";
                                    } else if (op_node.type == NodeType::Number) {
                                        out << "sb_" << name << ".append(L, clx::LValue(static_cast<double>("
                                            << op_node.as.number.val << ")));\n";
                                    } else if (op_node.type == NodeType::CallExpression
                                        || op_node.type == NodeType::Vararg) {
                                        out << "sb_" << name << ".append(L, ";
                                        emit_node(op_idx);
                                        out << ");\n";
                                    } else {
                                        out << "sb_" << name << ".append(L, ";
                                        emit_node(op_idx);
                                        out << ");\n";
                                    }
                                }
                                intercepted = true;
                            }
                        }
                    }
                }
                if (!intercepted) {
                    if (is_boxed)
                        out << "clx::upval_store(l_" << name << ", ";
                    else
                        out << "l_" << name << " = ";

                    bool lhs_is_native = false;
                    if (v_count > 0) {
                        lhs_is_native = !is_boxed
                            && std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                                != state.native_numbers.end();
                        if (lhs_is_native)
                            emit_native(ctx.block_statements[first_v]);
                        else
                            emit_node(ctx.block_statements[first_v]);
                    } else
                        out << "clx::LValue()";
                    out << (is_boxed ? ");\n" : ";\n");
                    if (lhs_is_native) {
                        for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                            if (it->name == name) {
                                if (it->has_intf) {
                                    if (v_count > 0) {
                                        out << "_intf_l_" << name << " = ("
                                            << int_flag_expr(ctx.block_statements[first_v], 1) << ");\n";
                                        if (it->has_ii)
                                            out << "_ii_l_" << name << " = ("
                                                << int_value_expr(ctx.block_statements[first_v], 1) << ");\n";
                                    } else {
                                        out << "_intf_l_" << name << " = false;\n";
                                        if (it->has_ii)
                                            out << "_ii_l_" << name << " = 0;\n";
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                        && !state.module_string_builders.count(name)) {
                        out << "    sb_" << name << ".clear();\n";
                    }
                }
            } else {
                size_t idx = state.string_pool_index.at(name);
                out << "L->_G->settable(cstr_[" << idx << "], ";
                if (v_count > 0)
                    emit_node(ctx.block_statements[first_v]);
                else
                    out << "clx::LValue()";
                out << ");\n";
            }
        } else if (t_node.type == NodeType::TableAccess) {
            std::string_view t_name;
            if (ctx.nodes[t_node.as.table_access.table].type == NodeType::Identifier) {
                t_name = std::string_view(ctx.nodes[t_node.as.table_access.table].as.ident.name,
                    ctx.nodes[t_node.as.table_access.table].as.ident.length);
            }

            if (v_count == 1) {
                uint32_t v_idx = ctx.block_statements[first_v];
                if (v_idx < ctx.nodes.size() && ctx.nodes[v_idx].type == NodeType::BinaryOp) {
                    auto &bin = ctx.nodes[v_idx].as.bin_op;
                    int bin_op = bin.op;
                    if (bin_op == static_cast<int>(BinaryOp::Add) || bin_op == static_cast<int>(BinaryOp::Mul)
                        || bin_op == static_cast<int>(BinaryOp::Sub) || bin_op == static_cast<int>(BinaryOp::Div)) {
                        uint32_t lhs_tbl = t_node.as.table_access.table;
                        uint32_t lhs_key = t_node.as.table_access.key;
                        int swap_limit
                            = (bin_op == static_cast<int>(BinaryOp::Sub) || bin_op == static_cast<int>(BinaryOp::Div))
                            ? 1
                            : 2;
                        for (int swap = 0; swap < swap_limit; ++swap) {
                            uint32_t ta_idx = swap ? bin.right : bin.left;
                            uint32_t const_idx = swap ? bin.left : bin.right;
                            if (ta_idx < ctx.nodes.size() && ctx.nodes[ta_idx].type == NodeType::TableAccess
                                && const_idx < ctx.nodes.size()
                                && (ctx.nodes[const_idx].type == NodeType::Integer
                                    || ctx.nodes[const_idx].type == NodeType::Number)) {
                                auto &ta = ctx.nodes[ta_idx].as.table_access;
                                bool tables_match = false;
                                if (lhs_tbl < ctx.nodes.size() && ta.table < ctx.nodes.size()
                                    && ctx.nodes[lhs_tbl].type == NodeType::Identifier
                                    && ctx.nodes[ta.table].type == NodeType::Identifier) {
                                    std::string_view lhs_tn(
                                        ctx.nodes[lhs_tbl].as.ident.name, ctx.nodes[lhs_tbl].as.ident.length);
                                    std::string_view rhs_tn(
                                        ctx.nodes[ta.table].as.ident.name, ctx.nodes[ta.table].as.ident.length);
                                    tables_match = (lhs_tn == rhs_tn);
                                }
                                bool keys_match = (lhs_key == ta.key)
                                    || (lhs_key < ctx.nodes.size() && ta.key < ctx.nodes.size()
                                        && ctx.nodes[lhs_key].type == NodeType::Identifier
                                        && ctx.nodes[ta.key].type == NodeType::Identifier
                                        && std::string_view(
                                               ctx.nodes[lhs_key].as.ident.name, ctx.nodes[lhs_key].as.ident.length)
                                            == std::string_view(
                                                ctx.nodes[ta.key].as.ident.name, ctx.nodes[ta.key].as.ident.length));
                                if (tables_match && keys_match) {
                                    bool zi_ok = lhs_tbl < ctx.nodes.size()
                                        && ctx.nodes[lhs_tbl].type == NodeType::Identifier
                                        && state.zero_index_tables.count({ owner_of_node(state, node_idx),
                                            std::string_view(ctx.nodes[lhs_tbl].as.ident.name,
                                                ctx.nodes[lhs_tbl].as.ident.length) });
                                    emitTableOp(bin_op, lhs_tbl, lhs_key, const_idx, zi_ok);
                                    return;
                                }
                            }
                        }
                    }
                }
            }

            std::string _hpt = hoisted_table_ptr(t_node.as.table_access.table);
            if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
                out << "{ size_t _n = static_cast<size_t>(";
                emit_native(t_node.as.table_access.key);
                out << "); if (_n > l_" << t_name << ".size()) l_" << t_name << ".resize(_n); l_" << t_name
                    << "[_n - 1] = ";
                if (v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    if (ctx.nodes[v_idx].type == NodeType::TrueLiteral)
                        out << "1.0";
                    else if (ctx.nodes[v_idx].type == NodeType::FalseLiteral)
                        out << "0.0";
                    else
                        emit_native(v_idx);
                } else
                    out << "0.0";
                out << "; }\n";
            } else if (state.bce_safe_nodes.count(t_idx)) {
                out << "{ ";
                if (!_hpt.empty()) {
                    out << "clx::LTable* _t" << t_idx << " = " << _hpt << ";";
                } else {
                    out << "clx::LValue _tb" << t_idx << " = ";
                    emit_node(t_node.as.table_access.table);
                    out << "; if (_tb" << t_idx << ".type != clx::ValueType::Table) clx::throw_index_error(L, _tb"
                        << t_idx << "); clx::LTable* _t" << t_idx << " = static_cast<clx::LTable*>(_tb" << t_idx
                        << ".as_pointer());";
                }
                bool _did_store = false;
                if (!_hpt.empty() && v_count > 0) {
                    uint32_t _vo = ctx.block_statements[first_v];
                    if (_vo < ctx.nodes.size() && ctx.nodes[_vo].type == NodeType::TableAccess) {
                        const auto &_va = ctx.nodes[_vo].as.table_access;
                        if (_va.table < ctx.nodes.size() && ctx.nodes[_va.table].type == NodeType::Identifier
                            && is_purely_integer_expr(ctx, state, _va.key)
                            && is_purely_integer_expr(ctx, state, t_node.as.table_access.key)) {
                            std::string _sp = hoisted_table_ptr(_va.table);
                            if (!_sp.empty()) {
                                out << "{ clx::LTable* _td" << t_idx << " = " << _hpt << "; clx::LTable* _ts" << t_idx
                                    << " = " << _sp << "; int64_t _kd" << t_idx << " = static_cast<int64_t>(";
                                emit_native(t_node.as.table_access.key);
                                out << "); int64_t _ks" << t_idx << " = static_cast<int64_t>(";
                                emit_native(_va.key);
                                out << "); if (_kd" << t_idx << " >= 1 && _ks" << t_idx
                                    << " >= 1 && static_cast<size_t>(_kd" << t_idx << ") - 1 < _td" << t_idx
                                    << "->array_cap && static_cast<size_t>(_ks" << t_idx << ") <= _ts" << t_idx
                                    << "->array_size) [[likely]] { clx::TValue _tv" << t_idx << " = _ts" << t_idx
                                    << "->array[_ks" << t_idx << " - 1]; clx::ValueType _tt" << t_idx << " = _ts"
                                    << t_idx << "->array_types[_ks" << t_idx << " - 1]; clx::gc_barrier_table(L, _td"
                                    << t_idx << ", clx::LValue(_tv" << t_idx << ", _tt" << t_idx
                                    << ")); if (static_cast<size_t>(_kd" << t_idx << ") - 1 > _td" << t_idx
                                    << "->array_size) clx::table_fill_gap(_td" << t_idx << ", _td" << t_idx
                                    << "->array_size, static_cast<size_t>(_kd" << t_idx << ") - 1); _td" << t_idx
                                    << "->array[_kd" << t_idx << " - 1] = _tv" << t_idx << "; _td" << t_idx
                                    << "->array_types[_kd" << t_idx << " - 1] = _tt" << t_idx
                                    << "; if (static_cast<size_t>(_kd" << t_idx << ") > _td" << t_idx
                                    << "->array_size) _td" << t_idx << "->array_size = static_cast<size_t>(_kd" << t_idx
                                    << "); } else { clx::LValue _sv" << t_idx << " = ";
                                emit_node(_vo);
                                out << "; clx::table_set(L, clx::LValue(clx::ValueType::Table, _td" << t_idx
                                    << "), clx::LValue(_kd" << t_idx << "), _sv" << t_idx << "); } } }\n";
                                _did_store = true;
                            }
                        }
                    }
                }
                if (!_did_store) {
                    out << " clx::LValue _sv" << t_idx << " = ";
                    if (v_count > 0)
                        emit_node(ctx.block_statements[first_v]);
                    else
                        out << "clx::LValue()";
                    out << "; size_t _k" << t_idx << " = static_cast<size_t>(";
                    emit_native(t_node.as.table_access.key);
                    out << "); if (_k" << t_idx << " - 1 < _t" << t_idx << "->array_cap) [[likely]] { ";
                    if (!(v_count > 0
                            && yields_number(
                                ctx, state, ctx.block_statements[first_v], nullptr, state.current_fast_func)))
                        out << "clx::gc_barrier_table(L, _t" << t_idx << ", _sv" << t_idx << "); ";
                    out << "if (_k" << t_idx << " - 1 > _t" << t_idx << "->array_size) clx::table_fill_gap(_t" << t_idx
                        << ", _t" << t_idx << "->array_size, _k" << t_idx << " - 1); _t" << t_idx << "->array[_k"
                        << t_idx << " - 1] = _sv" << t_idx << ".val; _t" << t_idx << "->array_types[_k" << t_idx
                        << " - 1] = _sv" << t_idx << ".type; if (_k" << t_idx << " > _t" << t_idx << "->array_size) _t"
                        << t_idx << "->array_size = _k" << t_idx
                        << "; } else "
                           "clx::table_set_int(L, clx::LValue(clx::ValueType::Table, _t"
                        << t_idx << "), _k" << t_idx << ", _sv" << t_idx << "); }\n";
                }
            } else {
                bool key_is_native = false;
                uint32_t k_idx = t_node.as.table_access.key;
                if (yields_number(ctx, state, k_idx, nullptr, state.current_fast_func))
                    key_is_native = true;

                if (key_is_native) {
                    out << "{ ";
                    if (!_hpt.empty()) {
                        out << "clx::LTable* _t = " << _hpt << ";";
                    } else {
                        out << "clx::LValue _b = ";
                        emit_node(t_node.as.table_access.table);
                        out << "; if (_b.type != clx::ValueType::Table) clx::throw_index_error(L, _b);";
                        out << " clx::LTable* _t = static_cast<clx::LTable*>(_b.as_pointer());";
                    }
                    out << " size_t _k = static_cast<size_t>(";
                    emit_native(k_idx);
                    out << "); if (_k - 1 < _t->array_size) { clx::LValue _sv = ";
                    if (v_count > 0)
                        emit_node(ctx.block_statements[first_v]);
                    else
                        out << "clx::LValue()";
                    bool lhs_val_numeric = v_count > 0 && ctx.block_statements[first_v] < ctx.nodes.size()
                        && yields_number(ctx, state, ctx.block_statements[first_v], nullptr, state.current_fast_func);
                    if (!lhs_val_numeric)
                        out << "; clx::gc_barrier_table(L, _t, _sv)";
                    out << "; _t->array[_k - 1] = _sv.val; _t->array_types[_k - 1] = _sv.type; } else "
                           "clx::table_set_int(L, clx::LValue(clx::ValueType::Table, _t), _k, ";
                    if (v_count > 0)
                        emit_node(ctx.block_statements[first_v]);
                    else
                        out << "clx::LValue()";
                    out << "); }\n";
                } else {
                    uint32_t kt = t_node.as.table_access.key;
                    if (kt < ctx.nodes.size() && ctx.nodes[kt].type == NodeType::String) {
                        uint32_t tbl_idx = t_node.as.table_access.table;
                        bool tbl_is_stable
                            = tbl_idx < ctx.nodes.size() && ctx.nodes[tbl_idx].type == NodeType::Identifier;
                        bool is_known_field = false;
                        if (tbl_is_stable) {
                            std::string_view _tn(ctx.nodes[tbl_idx].as.ident.name, ctx.nodes[tbl_idx].as.ident.length);
                            std::string_view _kn(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length);
                            auto _it = state.numeric_table_fields.find(
                                { owner_of_node(state, state.current_func_body), _tn });
                            if (_it != state.numeric_table_fields.end() && _it->second.count(_kn))
                                is_known_field = true;
                        }
                        int _cs_i = -1;
                        size_t _cstr_idx = state.string_pool_index.at(
                            std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length));
                        if (is_known_field) {
                            out << "clx::table_set_direct(L, ";
                            emit_node(t_node.as.table_access.table);
                            out << ", cstr_[" << _cstr_idx << "], ";
                            if (v_count > 0)
                                emit_node(ctx.block_statements[first_v]);
                            else
                                out << "clx::LValue()";
                            out << ");\n";
                        } else {
                            out << "clx::table_set(L, ";
                            emit_node(t_node.as.table_access.table);
                            out << ", cstr_[" << _cstr_idx << "], ";
                            if (v_count > 0)
                                emit_node(ctx.block_statements[first_v]);
                            else
                                out << "clx::LValue()";
                            out << ");\n";
                        }
                    } else {
                        if (v_count == 1) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            if (v_idx < ctx.nodes.size() && ctx.nodes[v_idx].type == NodeType::BinaryOp) {
                                auto &bin = ctx.nodes[v_idx].as.bin_op;
                                int bin_op = bin.op;
                                if (bin_op == static_cast<int>(BinaryOp::Add)
                                    || bin_op == static_cast<int>(BinaryOp::Mul)
                                    || bin_op == static_cast<int>(BinaryOp::Sub)
                                    || bin_op == static_cast<int>(BinaryOp::Div)) {
                                    uint32_t lhs_tbl = t_node.as.table_access.table;
                                    uint32_t lhs_key = t_node.as.table_access.key;
                                    int swap_limit = (bin_op == static_cast<int>(BinaryOp::Sub)
                                                         || bin_op == static_cast<int>(BinaryOp::Div))
                                        ? 1
                                        : 2;
                                    for (int swap = 0; swap < swap_limit; ++swap) {
                                        uint32_t ta_idx = swap ? bin.right : bin.left;
                                        uint32_t const_idx = swap ? bin.left : bin.right;
                                        if (ta_idx < ctx.nodes.size() && ctx.nodes[ta_idx].type == NodeType::TableAccess
                                            && const_idx < ctx.nodes.size()
                                            && (ctx.nodes[const_idx].type == NodeType::Integer
                                                || ctx.nodes[const_idx].type == NodeType::Number)) {
                                            auto &ta = ctx.nodes[ta_idx].as.table_access;
                                            bool tables_match = false;
                                            if (lhs_tbl < ctx.nodes.size() && ta.table < ctx.nodes.size()
                                                && ctx.nodes[lhs_tbl].type == NodeType::Identifier
                                                && ctx.nodes[ta.table].type == NodeType::Identifier) {
                                                std::string_view lhs_tn(ctx.nodes[lhs_tbl].as.ident.name,
                                                    ctx.nodes[lhs_tbl].as.ident.length);
                                                std::string_view rhs_tn(ctx.nodes[ta.table].as.ident.name,
                                                    ctx.nodes[ta.table].as.ident.length);
                                                tables_match = (lhs_tn == rhs_tn);
                                            }
                                            bool keys_match = (lhs_key == ta.key)
                                                || (lhs_key < ctx.nodes.size() && ta.key < ctx.nodes.size()
                                                    && ctx.nodes[lhs_key].type == ctx.nodes[ta.key].type
                                                    && ((ctx.nodes[lhs_key].type == NodeType::Identifier
                                                            && std::string_view(ctx.nodes[lhs_key].as.ident.name,
                                                                   ctx.nodes[lhs_key].as.ident.length)
                                                                == std::string_view(ctx.nodes[ta.key].as.ident.name,
                                                                    ctx.nodes[ta.key].as.ident.length))
                                                        || (ctx.nodes[lhs_key].type == NodeType::String
                                                            && std::string_view(ctx.nodes[lhs_key].as.string.text,
                                                                   ctx.nodes[lhs_key].as.string.length)
                                                                == std::string_view(ctx.nodes[ta.key].as.string.text,
                                                                    ctx.nodes[ta.key].as.string.length))));
                                            if (tables_match && keys_match) {
                                                bool zi_ok = lhs_tbl < ctx.nodes.size()
                                                    && ctx.nodes[lhs_tbl].type == NodeType::Identifier
                                                    && state.zero_index_tables.count({ owner_of_node(state, node_idx),
                                                        std::string_view(ctx.nodes[lhs_tbl].as.ident.name,
                                                            ctx.nodes[lhs_tbl].as.ident.length) });
                                                emitTableOp(bin_op, lhs_tbl, lhs_key, const_idx, zi_ok);
                                                return;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        out << "clx::table_set(L, ";
                        emit_node(t_node.as.table_access.table);
                        out << ", ";
                        emit_node(t_node.as.table_access.key);
                        out << ", ";
                        if (v_count > 0)
                            emit_node(ctx.block_statements[first_v]);
                        else
                            out << "clx::LValue()";
                        out << ");\n";
                    }
                }
            }
        }
        return;
    }

    if (last_is_call) {
        bool _mret_hoisted = false;
        for (uint32_t _j = 0; _j < t_count; ++_j) {
            uint32_t _t_idx = ctx.block_statements[first_t + _j];
            std::string_view _nm(ctx.nodes[_t_idx].as.ident.name, ctx.nodes[_t_idx].as.ident.length);
            if (state.hoisted_locals.count(_nm)) {
                _mret_hoisted = true;
                break;
            }
        }
        if (!_mret_hoisted)
            out << "clx::MultiValue _mret_" << node_idx << ";\n";
    }

    std::vector<bool> tmp_is_native(v_count, false);
    std::vector<bool> tmp_is_integer(v_count, false);
    if (v_count > 0) {
        out << "size_t _eg_base_" << node_idx << " = L->shadow_top;\n";
        for (size_t i = 0; i < v_count; ++i) {
            uint32_t v_idx = ctx.block_statements[first_v + i];

            bool intercepted = false;
            if (ctx.nodes[v_idx].type == NodeType::FunctionDef && i < t_count) {
                uint32_t t_idx = ctx.block_statements[first_t + i];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && !ctx.nodes[t_idx].as.ident.is_global) {
                    std::string_view fname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);

                    if (state.reassigned_vars.count(fname) == 0) {
                        bool is_fast = false;
                        if (state.native_return_funcs.count(fname) && state.func_param_native.count(fname)) {
                            is_fast = true;
                            for (bool p : state.func_param_native[fname])
                                if (!p)
                                    is_fast = false;
                        }
                        if (!is_fast && state.native_direct_funcs.count(fname)) {
                            emit_native_impl_def(fname, v_idx);
                        }

                        if (is_fast) {
                            out << "auto _fast_" << fname << "_impl = ";
                            state.emit_fast_lambda = true;
                            state.in_fast_function = true;
                            state.current_fast_func = fname;
                            emit_node(v_idx);
                            state.emit_fast_lambda = false;
                            state.in_fast_function = false;
                            state.current_fast_func = "";
                            out << ";\n";

                            out << "auto _fast_" << fname << " = [=]( ";
                            for (uint32_t a = 0; a < state.func_param_counts[fname]; ++a) {
                                out << "double p" << a << (a < state.func_param_counts[fname] - 1 ? ", " : "");
                            }
                            out << ") -> double { return _fast_" << fname << "_impl(_fast_" << fname << "_impl";
                            for (uint32_t a = 0; a < state.func_param_counts[fname]; ++a) {
                                out << ", p" << a;
                            }
                            out << "); };\n";

                            out << "auto _impl_" << fname
                                << " = [=](clx::LState* L, const clx::LValue* args, size_t arg_count) -> "
                                   "clx::MultiValue {\n";
                            bool _site3_masked = false;
                            if (state.int_returning_funcs.count(fname)) {
                                out << "    return clx::MultiValue(clx::LValue(static_cast<int64_t>(_fast_" << fname
                                    << "(";
                            } else if (state.int_preserving_masks.count(fname)) {
                                out << "    bool _bflag = ";
                                uint32_t _m = state.int_preserving_masks.at(fname);
                                bool _any_f = false;
                                for (uint32_t p = 0; p < state.func_param_counts[fname] && p < 32; ++p) {
                                    if (!(_m & (1u << p)))
                                        continue;
                                    out << (_any_f ? " && " : "") << "(" << p << " < arg_count) && args[" << p
                                        << "].type == clx::ValueType::Int64";
                                    _any_f = true;
                                }
                                if (!_any_f)
                                    out << "false";
                                out << ";\n";
                                out << "    return clx::MultiValue(clx::num_box(_fast_" << fname << "(";
                                _site3_masked = true;
                            } else {
                                out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << fname
                                    << "(";
                            }
                            for (uint32_t a = 0; a < state.func_param_counts[fname]; ++a) {
                                out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                if (a < state.func_param_counts[fname] - 1)
                                    out << ", ";
                            }
                            if (state.int_returning_funcs.count(fname))
                                out << "))));\n";
                            else if (_site3_masked)
                                out << "), _bflag));\n";
                            else
                                out << "))));\n";
                            out << "};\n";
                            state.fast_callables.insert(fname);
                        } else {
                            out << "auto _impl_" << fname << " = ";
                            state.emit_raw_lambda = true;
                            emit_node(v_idx);
                            state.emit_raw_lambda = false;
                            out << ";\n";
                        }

                        std::string cells_e = gc_cells_arg(std::string(fname));
                        out << "clx::LValue _tmp_" << node_idx << "_" << i << " = L->create_closure(_impl_" << fname
                            << ", static_cast<clx::LTable*>(_ENV.as_pointer())"
                            << (cells_e.empty() ? "" : ", " + cells_e) << ");\n";
                        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_tmp_" << node_idx << "_" << i
                            << ".val, &_tmp_" << node_idx << "_" << i << ".type);\n";
                        state.direct_callables.insert(fname);
                        intercepted = true;
                    }
                }
            }

            if (intercepted)
                continue;

            if (i == v_count - 1 && last_is_call) {

                const auto &lcnode = ctx.nodes[v_idx];
                bool lc_native = false;
                if (lcnode.type == NodeType::CallExpression
                    && native_call_eligible(
                        lcnode.as.call_expr.target, lcnode.as.call_expr.first_arg, lcnode.as.call_expr.arg_count)) {
                    out << "_mret_" << node_idx << " = ";
                    lc_native = try_emit_native_call(
                        lcnode.as.call_expr.target, lcnode.as.call_expr.first_arg, lcnode.as.call_expr.arg_count, true);
                    out << ";\n";
                }
                if (!lc_native) {
                    state.expect_multivalue = true;
                    out << "_mret_" << node_idx << " = ";
                    emit_node(v_idx);
                    out << ";\n";
                    state.expect_multivalue = false;
                }
                out << "for (size_t _eg_mri = 0; _eg_mri < _mret_" << node_idx << ".count; ++_eg_mri) {\n";
                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_mret_" << node_idx
                    << "[_eg_mri].val, &_mret_" << node_idx << "[_eg_mri].type);\n";
                out << "}\n";
            } else {
                bool t_is_n = false;
                bool v_is_n = yields_number(ctx, state, v_idx, nullptr, state.current_fast_func);
                if (i < t_count) {
                    uint32_t t_idx = ctx.block_statements[first_t + i];
                    if (ctx.nodes[t_idx].type == NodeType::Identifier) {
                        std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                        t_is_n = std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                            != state.native_numbers.end();
                    }
                }

                if (v_is_n)
                    t_is_n = true;

                if (t_is_n && v_is_n) {
                    tmp_is_native[i] = true;
                    std::string_view tname;
                    if (i < t_count) {
                        uint32_t t_idx2 = ctx.block_statements[first_t + i];
                        if (ctx.nodes[t_idx2].type == NodeType::Identifier)
                            tname
                                = std::string_view(ctx.nodes[t_idx2].as.ident.name, ctx.nodes[t_idx2].as.ident.length);
                    }
                    bool is_int_tmp = !tname.empty() && state.native_integers.count(tname) > 0;
                    if (!is_int_tmp) {
                        is_int_tmp = clx::is_purely_integer_expr(ctx, state, v_idx)
                            && !var_reassigned_non_int(tname, state.current_func_body, v_idx);
                    }
                    if (is_int_tmp) {
                        tmp_is_integer[i] = true;
                        out << "int64_t _tmp_" << node_idx << "_" << i << " = ";
                    } else {
                        out << "double _tmp_" << node_idx << "_" << i << " = ";
                    }
                    emit_native(v_idx);
                    out << ";\n";
                } else {
                    out << "clx::LValue _tmp_" << node_idx << "_" << i << " = ";
                    emit_node(v_idx);
                    out << ";\n";
                    out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_tmp_" << node_idx << "_" << i
                        << ".val, &_tmp_" << node_idx << "_" << i << ".type);\n";
                }
            }
        }
        out << "size_t _eg_mid_" << node_idx << " = L->shadow_top;\n";
    }

    if (is_local) {
        std::set<std::string_view> seen_in_decl;
        for (size_t i = 0; i < t_count; ++i) {
            uint32_t t_idx = ctx.block_statements[first_t + i];
            std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
            if (!seen_in_decl.insert(name).second)
                continue;
            bool in_native = std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                != state.native_numbers.end();
            bool val_is_native = (i < v_count && tmp_is_native[i]);
            bool is_n = in_native || val_is_native;
            bool is_cap = ctx.nodes[t_idx].as.ident.is_captured;
            bool site_b_created_csnap = false;

            if (is_cap) {
                if (state.constant_upvalues.count(name)) {
                    if (!state.hoisted_locals.count(name)) {
                        out << "clx::LValue l_" << name << ";\n";
                        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_" << name
                            << ".type);\n";
                        if (!state.pure_numeric_arrays.count(name)) {
                            out << "auto l_" << name << "_csnap = clx::make_upvalue(clx::LValue());\n";
                            state.const_snapshot_cells.push_back(std::string(name));
                            site_b_created_csnap = true;
                        }
                    }
                    locals.push_back({ name, false });
                    locals.back().has_csnap = site_b_created_csnap;
                } else {
                    if (!state.hoisted_locals.count(name)) {
                        out << "clx::LUpValue l_" << name << ";\n";
                        out << "l_" << name << " = clx::make_upvalue(clx::LValue());\n";
                        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << "->val, &l_" << name
                            << "->type);\n";
                    }
                    locals.push_back({ name, true });
                }
            } else if (is_n) {
                if (!in_native)
                    state.native_numbers.push_back(name);
                bool is_shadow_decl = false;
                for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                    if (it->name == name) {
                        is_shadow_decl = true;
                        break;
                    }
                }
                bool is_int_var = state.native_integers.count(name) > 0;
                if (!is_int_var && !is_shadow_decl && v_count > 0) {
                    uint32_t v_idx_check
                        = (first_v + i < ctx.block_statements.size()) ? ctx.block_statements[first_v + i] : 0xFFFFFFFF;
                    if (clx::is_purely_integer_expr(ctx, state, v_idx_check)
                        && !var_reassigned_non_int(name, state.current_func_body, v_idx_check)) {
                        is_int_var = true;
                        state.native_integers.insert(std::string(name));
                    }
                }
                if (!state.hoisted_locals.count(name)) {
                    if (is_int_var) {
                        out << "int64_t l_" << name << ";\n";
                        out << "l_" << name << " = INT64_C(0);\n";
                    } else {
                        out << "double l_" << name << ";\n";
                        out << "bool _intf_l_" << name << " = false;\n";
                        out << "int64_t _ii_l_" << name << " = 0;\n";
                        out << "l_" << name << " = 0.0;\n";
                    }
                }
                locals.push_back({ name, false });
                locals.back().has_intf = !is_int_var;
                locals.back().has_ii = !is_int_var;
            } else {
                if (!state.hoisted_locals.count(name)) {
                    out << "clx::LValue l_" << name << ";\n";
                    out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_" << name
                        << ".type);\n";
                }
                if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                    && !state.module_string_builders.count(name)) {
                    out << "clx::StringBuilder sb_" << name << ";\n";
                }
                locals.push_back({ name, false });
                if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                    && !state.module_string_builders.count(name)) {
                    locals.back().has_sb = true;
                }
            }
        }
    }

    for (size_t i = 0; i < t_count; ++i) {
        std::string val_str;
        std::string num_str;
        bool val_is_number = false;
        if (i < v_count) {
            uint32_t vv = ctx.block_statements[first_v + i];
            if (vv < ctx.nodes.size() && yields_number(ctx, state, vv, nullptr, state.current_fast_func))
                val_is_number = true;
        }

        if (i < v_count) {
            if (i == v_count - 1 && last_is_call) {
                val_str = "_mret_" + std::to_string(node_idx) + "[0]";
                num_str = val_str + ".as_number()";
            } else {
                if (tmp_is_native[i]) {
                    num_str = "_tmp_" + std::to_string(node_idx) + "_" + std::to_string(i);
                    if (tmp_is_integer[i]) {
                        val_str = "clx::LValue(static_cast<int64_t>(" + num_str + "))";
                    } else {
                        val_str = "clx::LValue(static_cast<double>(" + num_str + "))";
                    }
                } else {
                    val_str = "_tmp_" + std::to_string(node_idx) + "_" + std::to_string(i);
                    num_str = val_str + ".as_number()";
                }
            }
        } else if (last_is_call) {
            val_str = "(((" + std::to_string(i) + " - " + std::to_string(v_count - 1) + ") < _mret_"
                + std::to_string(node_idx) + ".count) ? _mret_" + std::to_string(node_idx) + "[" + std::to_string(i)
                + " - " + std::to_string(v_count - 1) + "] : clx::LValue())";
            num_str = val_str + ".as_number()";
        } else {
            val_str = "clx::LValue()";
            num_str = "0.0";
        }

        uint32_t t_idx = ctx.block_statements[first_t + i];
        const auto &t_node = ctx.nodes[t_idx];

        if (t_node.type == NodeType::Identifier) {
            std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
            bool is_n = std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                != state.native_numbers.end();
            bool is_boxed = false;
            bool is_loc = this->is_local(name, is_boxed);

            if (is_local && !t_node.as.ident.is_global && state.reassigned_vars.count(std::string(name)) == 0
                && i < v_count && !tmp_is_native[i]) {
                uint32_t _alias_v_idx2 = ctx.block_statements[first_v + i];
                if (_alias_v_idx2 < ctx.nodes.size() && ctx.nodes[_alias_v_idx2].type == NodeType::TableAccess) {
                    uint32_t _ht3 = ctx.nodes[_alias_v_idx2].as.table_access.table;
                    uint32_t _hk3 = ctx.nodes[_alias_v_idx2].as.table_access.key;
                    if (_ht3 < ctx.nodes.size() && _hk3 < ctx.nodes.size()
                        && ctx.nodes[_ht3].type == NodeType::Identifier && ctx.nodes[_hk3].type == NodeType::String) {
                        std::string_view _hm3(ctx.nodes[_ht3].as.ident.name, ctx.nodes[_ht3].as.ident.length);
                        std::string_view _hf3(ctx.nodes[_hk3].as.string.text, ctx.nodes[_hk3].as.string.length);
                        const char *_cf3 = lookup_builtin(_hm3, _hf3);
                        if (_cf3)
                            state.builtin_aliases[std::string(name)] = _cf3;
                    }
                }
            }

            if (is_local || is_loc) {
                if (is_local && is_boxed)
                    out << "clx::upval_store(l_" << name << ", " << val_str << ");\n";
                else if (!is_local && is_boxed)
                    out << "clx::upval_store(l_" << name << ", " << val_str << ");\n";
                else if (is_n)
                    out << "l_" << name << " = " << num_str << ";\n";
                else
                    out << "l_" << name << " = " << val_str << ";\n";
                if (is_n && !is_boxed && i < v_count && !tmp_is_native[i] && state.native_integers.count(name) == 0) {
                    uint32_t fv = ctx.block_statements[first_v + i];
                    out << "_intf_l_" << name << " = (" << int_flag_expr(fv, 1) << ");\n";
                    out << "_ii_l_" << name << " = (" << int_value_expr(fv, 1) << ");\n";
                }
                if (!is_n
                    && std::find_if(state.const_snapshot_cells.begin() + csnap_base, state.const_snapshot_cells.end(),
                           [&](const std::string &n) { return n == name; })
                        != state.const_snapshot_cells.end())
                    out << "*l_" << name << "_csnap = l_" << name << ";\n";
            } else if (is_global && v_count == 0) {
            } else if (t_node.as.ident.is_global) {
                out << "clx::set_env_var(L, _ENV, \"" << name << "\", " << val_str << ");\n";
            } else {
                size_t idx = state.string_pool_index.at(name);
                out << "L->_G->settable(cstr_[" << idx << "], " << val_str << ");\n";
            }
        } else if (t_node.type == NodeType::TableAccess) {
            std::string_view t_name;
            if (ctx.nodes[t_node.as.table_access.table].type == NodeType::Identifier) {
                t_name = std::string_view(ctx.nodes[t_node.as.table_access.table].as.ident.name,
                    ctx.nodes[t_node.as.table_access.table].as.ident.length);
            }

            std::string _hpt = hoisted_table_ptr(t_node.as.table_access.table);
            if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
                out << "l_" << t_name << "[static_cast<size_t>(";
                emit_native(t_node.as.table_access.key);
                out << ") - 1] = " << num_str << ";\n";
            } else if (state.bce_safe_nodes.count(t_idx)) {
                out << "{ ";
                if (!_hpt.empty()) {
                    out << "clx::LTable* _t" << t_idx << " = " << _hpt << ";";
                } else {
                    out << "clx::LValue _tb" << t_idx << " = ";
                    emit_node(t_node.as.table_access.table);
                    out << "; if (_tb" << t_idx << ".type != clx::ValueType::Table) clx::throw_index_error(L, _tb"
                        << t_idx << "); clx::LTable* _t" << t_idx << " = static_cast<clx::LTable*>(_tb" << t_idx
                        << ".as_pointer());";
                }
                out << " size_t _k" << t_idx << " = static_cast<size_t>(";
                emit_native(t_node.as.table_access.key);
                out << "); if (_k" << t_idx << " - 1 < _t" << t_idx << "->array_cap) [[likely]] { clx::LValue _sv"
                    << t_idx << " = " << val_str << "; ";
                if (!val_is_number)
                    out << "clx::gc_barrier_table(L, _t" << t_idx << ", _sv" << t_idx << "); ";
                out << "if (_k" << t_idx << " - 1 > _t" << t_idx << "->array_size) clx::table_fill_gap(_t" << t_idx
                    << ", _t" << t_idx << "->array_size, _k" << t_idx << " - 1); _t" << t_idx << "->array[_k" << t_idx
                    << " - 1] = _sv" << t_idx << ".val; _t" << t_idx << "->array_types[_k" << t_idx << " - 1] = _sv"
                    << t_idx << ".type; if (_k" << t_idx << " > _t" << t_idx << "->array_size) _t" << t_idx
                    << "->array_size = _k" << t_idx
                    << "; } else clx::table_set_int(L, clx::LValue(clx::ValueType::Table, _t" << t_idx << "), _k"
                    << t_idx << ", " << val_str << "); }\n";
            } else {
                bool key_is_native = false;
                uint32_t k_idx = t_node.as.table_access.key;
                if (yields_number(ctx, state, k_idx, nullptr, state.current_fast_func))
                    key_is_native = true;

                if (key_is_native) {
                    out << "{ ";
                    if (!_hpt.empty()) {
                        out << "clx::LTable* _t = " << _hpt << ";";
                    } else {
                        out << "clx::LValue _b = ";
                        emit_node(t_node.as.table_access.table);
                        out << "; if (_b.type != clx::ValueType::Table) clx::throw_index_error(L, _b);";
                        out << " clx::LTable* _t = static_cast<clx::LTable*>(_b.as_pointer());";
                    }
                    out << " size_t _k = static_cast<size_t>(";
                    emit_native(k_idx);
                    out << "); if (_k - 1 < _t->array_size) [[likely]] { clx::LValue _svw = " << val_str << "; ";
                    if (!val_is_number)
                        out << "clx::gc_barrier_table(L, _t, _svw); ";
                    out << "_t->array[_k - 1] = _svw.val; _t->array_types[_k - 1] = "
                           "_svw.type; } else clx::table_set_int(L, clx::LValue(clx::ValueType::Table, _t), _k, "
                        << val_str << "); }\n";
                } else {
                    uint32_t kt2 = t_node.as.table_access.key;
                    if (kt2 < ctx.nodes.size() && ctx.nodes[kt2].type == NodeType::String) {
                        uint32_t tbl_idx = t_node.as.table_access.table;
                        bool tbl_is_stable
                            = tbl_idx < ctx.nodes.size() && ctx.nodes[tbl_idx].type == NodeType::Identifier;
                        bool is_known_field = false;
                        if (tbl_is_stable) {
                            std::string_view _tn(ctx.nodes[tbl_idx].as.ident.name, ctx.nodes[tbl_idx].as.ident.length);
                            std::string_view _kn(ctx.nodes[kt2].as.string.text, ctx.nodes[kt2].as.string.length);
                            auto _it = state.numeric_table_fields.find(
                                { owner_of_node(state, state.current_func_body), _tn });
                            if (_it != state.numeric_table_fields.end() && _it->second.count(_kn))
                                is_known_field = true;
                        }
                        size_t _cstr_idx = state.string_pool_index.at(
                            std::string_view(ctx.nodes[kt2].as.string.text, ctx.nodes[kt2].as.string.length));
                        if (is_known_field) {
                            out << "clx::table_set_direct(L, ";
                            emit_node(t_node.as.table_access.table);
                            out << ", cstr_[" << _cstr_idx << "], " << val_str << ");\n";
                        } else {
                            out << "clx::table_set(L, ";
                            emit_node(t_node.as.table_access.table);
                            out << ", cstr_[" << _cstr_idx << "], " << val_str << ");\n";
                        }
                    } else {
                        out << "clx::table_set(L, ";
                        emit_node(t_node.as.table_access.table);
                        out << ", ";
                        emit_node(t_node.as.table_access.key);
                        out << ", " << val_str << ");\n";
                    }
                }
            }
        }
    }

    if (is_local) {
        for (size_t i = 0; i < t_count; ++i) {
            uint32_t t_idx = ctx.block_statements[first_t + i];
            const auto &t_node = ctx.nodes[t_idx];
            if (t_node.type == NodeType::Identifier && t_node.as.ident.attr == clx::Attribute::Close) {
                std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
                bool is_n = std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                    != state.native_numbers.end();
                if (is_n) {
                    out << "clx::CloseGuard __cg_" << node_idx << "_" << i << "(L, clx::LValue(l_" << name << "));\n";
                } else if (t_node.as.ident.is_captured) {
                    out << "clx::CloseGuard __cg_" << node_idx << "_" << i << "(L, *l_" << name << ");\n";
                } else {
                    out << "clx::CloseGuard __cg_" << node_idx << "_" << i << "(L, l_" << name << ");\n";
                }
            }
        }
    }

    if (v_count > 0) {
        out << "if (L->shadow_top > _eg_mid_" << node_idx << ") {\n";
        out << "size_t _eg_n = L->shadow_top - _eg_mid_" << node_idx << ";\n";
        out << "for (size_t _eg_i = 0; _eg_i < _eg_n; ++_eg_i) {\n";
        out << "L->shadow_stack[_eg_base_" << node_idx << " + _eg_i] = L->shadow_stack[_eg_mid_" << node_idx
            << " + _eg_i];\n";
        out << "}\n";
        out << "L->shadow_top = _eg_base_" << node_idx << " + _eg_n;\n";
        out << "} else {\n";
        out << "L->shadow_top = _eg_base_" << node_idx << ";\n";
        out << "}\n";
    }
}

//------------------ emitTableConstructor: handles NodeType::TableConstructor
void CodeEmitter::emitTableConstructor(const ASTNode &node, uint32_t node_idx) {
    bool _is_arena = state.current_arena_func != 0xFFFFFFFF && state.arena_safe_table_nodes.count(node_idx) > 0;
    uint32_t implicit_count = 0;
    uint32_t explicit_count = 0;
    bool any_direct_item = false;
    for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
        uint32_t k = ctx.block_statements[node.as.table_cons.first_item + i * 2];
        uint32_t v = ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1];
        if (k != 0xFFFFFFFF) {
            ++explicit_count;
            continue;
        }
        ++implicit_count;
        bool final_multi = i == node.as.table_cons.count - 1
            && (ctx.nodes[v].type == NodeType::Vararg || ctx.nodes[v].type == NodeType::CallExpression);
        if (!final_multi)
            any_direct_item = true;
    }
    bool presize_ok = false;
    if (state.table_presize.count(node_idx)) {
        uint32_t nidx = state.table_presize[node_idx];
        auto check_declared = [&](auto &self, uint32_t ni) -> bool {
            if (ni >= ctx.nodes.size())
                return true;
            const auto &n = ctx.nodes[ni];
            if (n.type == NodeType::Identifier) {
                std::string_view nm(n.as.ident.name, n.as.ident.length);
                bool dummy = false;
                return this->is_local(nm, dummy);
            }
            if (n.type == NodeType::BinaryOp)
                return self(self, n.as.bin_op.left) && self(self, n.as.bin_op.right);
            if (n.type == NodeType::UnaryOp)
                return self(self, n.as.unary_op.expr);
            if (n.type == NodeType::Number || n.type == NodeType::Integer)
                return true;
            return false;
        };
        presize_ok = check_declared(check_declared, nidx);
    }
    bool can_direct = any_direct_item && !presize_ok && !_is_arena;
    out << "([&]() {\nclx::LValue _t = ";
    if (_is_arena) {
        out << "clx::arena_create_table(L, &_arena, ";
    } else {
        out << "L->create_table(";
    }
    if (presize_ok) {
        out << "static_cast<size_t>(";
        emit_native(state.table_presize[node_idx]);
        out << ")";
    } else {
        out << implicit_count;
    }
    if (_is_arena) {
        size_t hsize_pow2 = 8;
        while (hsize_pow2 < explicit_count)
            hsize_pow2 <<= 1;
        out << ", " << hsize_pow2 << ");\n";
    } else {
        out << ", " << explicit_count << ");\nL->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_t.val, &_t.type);\n";
    }
    if (can_direct)
        out << "clx::LTable* _ta = static_cast<clx::LTable*>(_t.as_pointer());\n";
    uint32_t implicit_seq = 0;
    for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
        uint32_t k = ctx.block_statements[node.as.table_cons.first_item + i * 2];
        uint32_t v = ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1];
        bool final_multi = false;
        if (k == 0xFFFFFFFF) {
            ++implicit_seq;
            final_multi = i == node.as.table_cons.count - 1
                && (ctx.nodes[v].type == NodeType::Vararg || ctx.nodes[v].type == NodeType::CallExpression);
        }
        if (final_multi) {
            state.expect_multivalue = true;
            out << "clx::MultiValue _mret_" << node_idx << " = ";
            emit_node(v);
            out << ";\n";
            state.expect_multivalue = false;
            out << "for (size_t _vi = 0; _vi < _mret_" << node_idx << ".count; ++_vi) {\n";
            out << "  L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_mret_" << node_idx
                << "[_vi].val, "
                   "&_mret_"
                << node_idx << "[_vi].type);\n";
            out << "  static_cast<clx::LTable*>(_t.as_pointer())->settable(";
            out << "clx::LValue(static_cast<double>(" << implicit_seq << " + _vi)), _mret_" << node_idx << "[_vi]);\n";
            out << "  L->shadow_top--;\n";
            out << "}\n";
        } else if (k == 0xFFFFFFFF && can_direct) {
            out << "{ clx::LValue _tv = ";
            emit_node(v);
            out << "; L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_tv.val, &_tv.type); _ta->array["
                << (implicit_seq - 1) << "] = _tv.val; _ta->array_types[" << (implicit_seq - 1)
                << "] = _tv.type; L->shadow_top--; }\n";
        } else {
            out << "{\n";
            out << "clx::LValue _tv = ";
            emit_node(v);
            out << ";\n";
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_tv.val, &_tv.type);\n";
            if (k == 0xFFFFFFFF) {
                out << "static_cast<clx::LTable*>(_t.as_pointer())->settable(clx::LValue(static_cast<double>("
                    << implicit_seq << ")), _tv);\n";
            } else {
                out << "clx::LValue _tk = ";
                emit_node(k);
                out << ";\n";
                out << "static_cast<clx::LTable*>(_t.as_pointer())->settable(_tk, _tv);\n";
            }
            out << "L->shadow_top--;\n";
            out << "}\n";
        }
    }
    if (!_is_arena)
        out << "L->shadow_top--;\n";
    out << "return _t;\n}())";
}

}
