// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  expressions.cpp · Expression & call emit   │
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

//------------------ emit_native: emits an expression coerced to a raw C++ double
void CodeEmitter::emit_native(uint32_t n_idx) {

    if (yields_number(ctx, state, n_idx, nullptr, state.current_fast_func)) {
        const auto &n = ctx.nodes[n_idx];
        if (n.type == NodeType::IntrinsicCall) {
            const char *_cn = n.as.intrinsic_call.cname;
            if (strcmp(_cn, "__clx_deg") == 0 || strcmp(_cn, "__clx_rad") == 0) {
                if (n.as.intrinsic_call.arg_count > 0) {
                    emit_native(ctx.block_statements[n.as.intrinsic_call.first_arg]);
                    out << (_cn[7] == 'd' ? " * 57.29577951308232" : " * 0.017453292519943295");
                } else
                    out << "0.0";
                return;
            }
            if (strcmp(_cn, "std::log") == 0 && n.as.intrinsic_call.arg_count > 1) {
                out << "std::log(";
                emit_native(ctx.block_statements[n.as.intrinsic_call.first_arg]);
                out << ") / std::log(";
                emit_native(ctx.block_statements[n.as.intrinsic_call.first_arg + 1]);
                out << ")";
                return;
            }
            out << _cn << "(";
            if (n.as.intrinsic_call.arg_count > 0) {
                emit_native(ctx.block_statements[n.as.intrinsic_call.first_arg]);
                if (n.as.intrinsic_call.arg_count > 1
                    && (strcmp(_cn, "std::fmod") == 0 || strcmp(_cn, "std::atan2") == 0
                        || strcmp(_cn, "std::pow") == 0)) {
                    out << ", ";
                    emit_native(ctx.block_statements[n.as.intrinsic_call.first_arg + 1]);
                }
            } else
                out << "0.0";
            out << ")";
            return;
        }
        if (n.type == NodeType::Number) {
            double d = n.as.number.val;
            char buf[64];
            int len = std::snprintf(buf, sizeof(buf), "%.17g", d);
            out << buf;
            if (buf[strspn(buf, "-0123456789")] == '\0' && len < 62) {
                out << ".0";
            }
            return;
        }
        if (n.type == NodeType::Integer) {
            out << "static_cast<int64_t>(" << n.as.integer.val << ")";
            return;
        }
        if (n.type == NodeType::Identifier) {
            std::string_view name(n.as.ident.name, n.as.ident.length);
            if (state.global_constants.count(name)) {
                out << state.global_constants[name];
                return;
            }
            if (state.int_typed_locals.count(name)) {
                bool is_boxed_tmp = false;
                std::string_view cpp_tmp;
                bool is_loc_tmp = this->is_local(name, is_boxed_tmp, cpp_tmp);
                std::string_view emit_tmp = cpp_tmp.empty() ? name : cpp_tmp;
                out << "static_cast<size_t>(l_" << emit_tmp << ".as_integer())";
                return;
            }
            if (std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                != state.native_numbers.end()) {
                bool is_boxed = false;
                std::string_view cpp_name;
                this->is_local(name, is_boxed, cpp_name);
                std::string_view emit_name = cpp_name.empty() ? name : cpp_name;
                if (is_boxed)
                    out << "(*l_" << emit_name << ").as_number()";
                else
                    out << "l_" << emit_name;
                return;
            }
        }
        if (n.type == NodeType::BinaryOp) {
            int op = n.as.bin_op.op;
            if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Mul)
                && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.right)) {
                out << (op == 1 ? "clx::int_add(" : op == 2 ? "clx::int_sub(" : "clx::int_mul(");
                out << "static_cast<int64_t>(";
                emit_native(n.as.bin_op.left);
                out << "), static_cast<int64_t>(";
                emit_native(n.as.bin_op.right);
                out << "))";
                return;
            }
            if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) {
                if (op == static_cast<int>(BinaryOp::Div)) {
                    if (is_zero_number_literal(ctx, n.as.bin_op.right)) {
                        out << "(static_cast<double>(";
                        emit_native(n.as.bin_op.left);
                        out << ") * std::numeric_limits<double>::infinity())";
                    } else {
                        out << "(static_cast<double>(";
                        emit_native(n.as.bin_op.left);
                        out << ") / static_cast<double>(";
                        emit_native(n.as.bin_op.right);
                        out << "))";
                    }
                    return;
                }
                out << "(";
                emit_native(n.as.bin_op.left);
                if (op == 1)
                    out << " + ";
                if (op == 2)
                    out << " - ";
                if (op == 3)
                    out << " * ";
                emit_native(n.as.bin_op.right);
                out << ")";
                return;
            }
            if (op == static_cast<int>(BinaryOp::FloorDiv)) {
                if (clx::is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                    && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.right)) {
                    out << "clx::int_floor_div(static_cast<int64_t>(";
                    emit_native(n.as.bin_op.left);
                    out << "), static_cast<int64_t>(";
                    emit_native(n.as.bin_op.right);
                    out << "))";
                } else {
                    out << "std::floor((";
                    emit_native(n.as.bin_op.left);
                    out << ") / (";
                    emit_native(n.as.bin_op.right);
                    out << "))";
                }
                return;
            }
            if (op == static_cast<int>(BinaryOp::Mod)) {
                if (clx::is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                    && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.right)) {
                    out << "clx::int_floor_mod(static_cast<int64_t>(";
                    emit_native(n.as.bin_op.left);
                    out << "), static_cast<int64_t>(";
                    emit_native(n.as.bin_op.right);
                    out << "))";
                } else {
                    out << "clx::fmod_floor(";
                    emit_native(n.as.bin_op.left);
                    out << ", ";
                    emit_native(n.as.bin_op.right);
                    out << ")";
                }
                return;
            }
        }
        if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Minus)) {
            out << "(-(";
            emit_native(n.as.unary_op.expr);
            out << "))";
            return;
        }
        if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Len)) {
            out << "clx::len(L, ";
            emit_node(n.as.unary_op.expr);
            out << ").as_number()";
            return;
        }
        if (n.type == NodeType::ParenExpression) {
            out << "(";
            emit_native(n.as.paren_expr.expr);
            out << ")";
            return;
        }
        if (n.type == NodeType::TableAccess) {
            {
                auto hit = state.hoisted_lookups.find(n_idx);
                if (hit != state.hoisted_lookups.end()) {
                    out << "(" << hit->second << ").as_number()";
                    return;
                }
            }
            std::string_view t_name;
            if (ctx.nodes[n.as.table_access.table].type == NodeType::Identifier) {
                t_name = std::string_view(ctx.nodes[n.as.table_access.table].as.ident.name,
                    ctx.nodes[n.as.table_access.table].as.ident.length);
            }
            if (ctx.nodes[n.as.table_access.key].type == NodeType::String) {
                if (!t_name.empty()) {
                    auto it = state.numeric_table_fields.find({ owner_of_node(state, n_idx), t_name });
                    if (it != state.numeric_table_fields.end()) {
                        std::string_view fn(ctx.nodes[n.as.table_access.key].as.string.text,
                            ctx.nodes[n.as.table_access.key].as.string.length);
                        if (it->second.count(fn)) {
                            size_t idx = state.string_pool_index.at(fn);
                            out << "clx::table_get(L, ";
                            emit_node(n.as.table_access.table);
                            out << ", cstr_[" << idx << "]).as_number()";
                            return;
                        }
                    }
                }
                out << "(";
                emit_node(n_idx);
                out << ").as_number()";
                return;
            }
            if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
                out << "l_" << t_name << "[static_cast<size_t>(";
                emit_native(n.as.table_access.key);
                out << ") - 1]";
                return;
            }
            if (state.bce_safe_nodes.count(n_idx)) {
                out << "([&](){ clx::LValue _tb" << n_idx << " = ";
                emit_node(n.as.table_access.table);
                out << "; if (_tb" << n_idx << ".type != clx::ValueType::Table) clx::throw_index_error(L, _tb" << n_idx
                    << "); clx::LTable* _t" << n_idx << " = static_cast<clx::LTable*>(_tb" << n_idx
                    << ".as_pointer()); size_t _k" << n_idx << " = static_cast<size_t>(";
                emit_native(n.as.table_access.key);
                out << "); return (_k" << n_idx << " - 1 < _t" << n_idx << "->array_size) ? clx::LValue(_t" << n_idx
                    << "->array[_k" << n_idx << " - 1], _t" << n_idx << "->array_types[_k" << n_idx
                    << " - 1]) : clx::table_get_int(L, _tb" << n_idx << ", _k" << n_idx << "); }()).as_number()";
                return;
            }
            bool pure_t = ctx.nodes[n.as.table_access.table].type == NodeType::Identifier;
            if (pure_t) {
                out << "([&](){ clx::LValue _b" << n_idx << " = ";
                emit_node(n.as.table_access.table);
                out << "; if (_b" << n_idx << ".type != clx::ValueType::Table) clx::throw_index_error(L, _b" << n_idx
                    << "); size_t _idx" << n_idx << " = static_cast<size_t>(";
                emit_native(n.as.table_access.key);
                out << ") - 1; auto* _t" << n_idx << " = static_cast<clx::LTable*>(_b" << n_idx
                    << ".as_pointer()); return (_idx" << n_idx << " < _t" << n_idx << "->array_size) ? clx::LValue(_t"
                    << n_idx << "->array[_idx" << n_idx << "], _t" << n_idx << "->array_types[_idx" << n_idx
                    << "]).as_number() : clx::table_get_int(L, ";
                emit_node(n.as.table_access.table);
                out << ", _idx" << n_idx << " + 1).as_number(); }())";
                return;
            }
        }
        if (n.type == NodeType::CallExpression) {
            bool is_fast = false;
            std::string_view fname;
            uint32_t tgt = n.as.call_expr.target;
            if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                if (state.fast_callables.count(fname)
                    || (!state.current_fast_func.empty() && fname == state.current_fast_func))
                    is_fast = true;
            }
            if (is_fast) {
                if (fname == state.current_fast_func) {
                    out << "self(self";
                    if (n.as.call_expr.arg_count > 0 || state.func_param_counts[fname] > 0)
                        out << ", ";
                } else {
                    out << "_fast_" << fname << "(";
                }
                for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) {
                    emit_native(ctx.block_statements[n.as.call_expr.first_arg + i]);
                    if (i < n.as.call_expr.arg_count - 1)
                        out << ", ";
                }
                for (uint32_t i = n.as.call_expr.arg_count; i < state.func_param_counts[fname]; ++i) {
                    if (i > 0 || n.as.call_expr.arg_count > 0)
                        out << ", ";
                    out << "0.0";
                }
                out << ")";
                return;
            }
        }
    }
    out << "(";
    emit_node(n_idx);
    out << ").as_number()";
}

//------------------ emit_condition: emits a boolean C++ expression for use in if/while
void CodeEmitter::emit_condition(uint32_t c_idx) {

    if (c_idx == 0xFFFFFFFF || c_idx >= ctx.nodes.size()) {
        out << "false";
        return;
    }
    const auto &c = ctx.nodes[c_idx];
    if (c.type == NodeType::TableAccess) {
        std::string_view t_name;
        if (ctx.nodes[c.as.table_access.table].type == NodeType::Identifier) {
            t_name = std::string_view(
                ctx.nodes[c.as.table_access.table].as.ident.name, ctx.nodes[c.as.table_access.table].as.ident.length);
        }
        if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
            out << "l_" << t_name << "[static_cast<size_t>(";
            emit_native(c.as.table_access.key);
            out << ") - 1] != 0.0";
            return;
        }
    }
    if (c.type == NodeType::BinaryOp) {
        int op = c.as.bin_op.op;
        if (op == static_cast<int>(BinaryOp::And) || op == static_cast<int>(BinaryOp::Or)) {
            out << "(";
            emit_condition(c.as.bin_op.left);
            out << (op == static_cast<int>(BinaryOp::And) ? " && " : " || ");
            emit_condition(c.as.bin_op.right);
            out << ")";
            return;
        }
        if (op >= static_cast<int>(BinaryOp::Eq) && op <= static_cast<int>(BinaryOp::Ne)) {
            bool left_native = yields_number(ctx, state, c.as.bin_op.left, nullptr, state.current_fast_func);
            bool right_native = yields_number(ctx, state, c.as.bin_op.right, nullptr, state.current_fast_func);
            if (left_native && right_native) {
                static const char *ops[] = { "", "", "", "", "", " == ", " < ", " > ", " <= ", " >= ", " != " };
                emit_native(c.as.bin_op.left);
                out << ops[op];
                emit_native(c.as.bin_op.right);
                return;
            }
        }
    }
    if (c.type == NodeType::UnaryOp && c.as.unary_op.op == static_cast<int>(UnaryOp::Not)) {
        out << "!(";
        emit_condition(c.as.unary_op.expr);
        out << ")";
        return;
    }
    if (c.type == NodeType::ParenExpression) {
        emit_condition(c.as.paren_expr.expr);
        return;
    }
    out << "(";
    emit_node(c_idx);
    out << ").as_bool()";
}

//------------------ emitIntrinsicCall: handles NodeType::IntrinsicCall
void CodeEmitter::emitIntrinsicCall(const ASTNode &node, uint32_t node_idx) {
    const char *_cn = node.as.intrinsic_call.cname;
    if (strcmp(_cn, "__clx_type") == 0) {
        uint32_t va = ctx.block_statements[node.as.intrinsic_call.first_arg];
        out << "([&](){ static const char* "
               "_tn[]={\"nil\",\"boolean\",\"number\",\"number\",\"string\",\"table\",\"function\",\"userdata\","
               "\"thread\"}; return clx::LValue(L->intern_string(_tn[static_cast<uint8_t>(";
        emit_node(va);
        out << ".type)])); }())";
    } else if (strcmp(_cn, "__clx_tostring") == 0) {
        uint32_t va = ctx.block_statements[node.as.intrinsic_call.first_arg];
        out << "clx::LValue(L->intern_string((";
        emit_node(va);
        out << ").to_string(L)))";
    } else {
        out << "clx::LValue(static_cast<double>(";
        if (strcmp(_cn, "__clx_deg") == 0 || strcmp(_cn, "__clx_rad") == 0) {
            emit_native(ctx.block_statements[node.as.intrinsic_call.first_arg]);
            out << (_cn[7] == 'd' ? " * 57.29577951308232" : " * 0.017453292519943295");
        } else if (strcmp(_cn, "std::log") == 0 && node.as.intrinsic_call.arg_count > 1) {
            out << "std::log(";
            emit_native(ctx.block_statements[node.as.intrinsic_call.first_arg]);
            out << ") / std::log(";
            emit_native(ctx.block_statements[node.as.intrinsic_call.first_arg + 1]);
            out << ")";
        } else {
            out << _cn << "(";
            if (node.as.intrinsic_call.arg_count > 0) {
                emit_native(ctx.block_statements[node.as.intrinsic_call.first_arg]);
                if (node.as.intrinsic_call.arg_count > 1
                    && (strcmp(_cn, "std::fmod") == 0 || strcmp(_cn, "std::atan2") == 0
                        || strcmp(_cn, "std::pow") == 0)) {
                    out << ", ";
                    emit_native(ctx.block_statements[node.as.intrinsic_call.first_arg + 1]);
                }
            } else {
                out << "0.0";
            }
            out << ")";
        }
        out << "))";
    }
}

//------------------ impl_call: call expression for a direct-callable's impl; heap holder cells are deref-ed
std::string CodeEmitter::impl_call(std::string_view fname) {
    if (state.holder_cell_callables.count(fname))
        return "(*_impl_" + std::string(fname) + ")";
    return "_impl_" + std::string(fname);
}

//------------------ gc_cells_arg: builds the strong-ref initializer list for create_closure
std::string CodeEmitter::gc_cells_arg(const std::string &exclude) {
    std::string out_list;
    std::unordered_set<std::string> seen;
    auto push = [&](const std::string &expr) {
        if (!seen.insert(expr).second)
            return;
        if (!out_list.empty())
            out_list += ", ";
        out_list += expr;
    };
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (it->is_boxed) {
            if (!exclude.empty() && it->cpp_name == exclude)
                continue;
            push("l_" + (it->cpp_name.empty() ? std::string(it->name) : it->cpp_name));
        } else if (it->has_csnap) {
            push("l_" + (it->cpp_name.empty() ? std::string(it->name) : it->cpp_name) + "_csnap");
        } else if (state.holder_cell_callables.count(it->name)) {
            push("l_" + std::string(it->name) + "_cell");
        }
    }
    if (out_list.empty())
        return "";
    return "std::vector<clx::LUpValue>{ " + out_list + " }";
}

//------------------ emitCallExpression: handles NodeType::CallExpression
void CodeEmitter::emitCallExpression(const ASTNode &node, uint32_t node_idx) {
    bool is_direct = false;
    bool is_fast = false;
    std::string_view fname;
    uint32_t tgt = node.as.call_expr.target;

    bool is_method_call = false;
    if (ctx.nodes[tgt].type == NodeType::TableAccess) {
        if (node.as.call_expr.arg_count > 0
            && ctx.block_statements[node.as.call_expr.first_arg] == ctx.nodes[tgt].as.table_access.table) {
            is_method_call = true;
        }
    }

    if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
        fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
        if (state.fast_callables.count(fname) || (!state.current_fast_func.empty() && fname == state.current_fast_func))
            is_fast = true;
        else if (state.direct_callables.count(fname))
            is_direct = true;
    }

    bool want_multi = state.expect_multivalue;
    state.expect_multivalue = false;

    if (is_fast) {
        if (want_multi)
            out << "clx::MultiValue(";
        bool masked = state.int_preserving_masks.count(fname) > 0 && state.int_preserving_masks.at(fname) != 0;
        if (state.int_returning_funcs.count(fname))
            out << "clx::LValue(static_cast<int64_t>(";
        else if (masked)
            out << "clx::num_box(";
        else
            out << "clx::LValue(static_cast<double>(";
        if (fname == state.current_fast_func) {
            out << "self(self";
            if (node.as.call_expr.arg_count > 0 || state.func_param_counts[fname] > 0)
                out << ", ";
        } else {
            out << "_fast_" << fname << "(";
        }

        for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i) {
            emit_native(ctx.block_statements[node.as.call_expr.first_arg + i]);
            if (i < node.as.call_expr.arg_count - 1)
                out << ", ";
        }
        for (uint32_t i = node.as.call_expr.arg_count; i < state.func_param_counts[fname]; ++i) {
            if (i > 0 || node.as.call_expr.arg_count > 0)
                out << ", ";
            out << "0.0";
        }
        if (state.int_returning_funcs.count(fname))
            out << ")))";
        else if (masked) {
            std::string bf = fast_call_box_flag(fname, node.as.call_expr.first_arg, node.as.call_expr.arg_count);
            out << "), " << bf << ")";
        } else
            out << ")))";
        if (want_multi)
            out << ")";
        return;
    }

    //------------------ native-direct call: the set is exact-1-only, so a

    if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
        && native_call_eligible(node.as.call_expr.target, node.as.call_expr.first_arg, node.as.call_expr.arg_count)) {
        if (want_multi)
            out << "clx::MultiValue(";
        try_emit_native_call(node.as.call_expr.target, node.as.call_expr.first_arg, node.as.call_expr.arg_count, false);
        if (want_multi)
            out << ")";
        return;
    }

    bool last_expands = false;
    uint32_t last_arg = 0xFFFFFFFF;
    if (node.as.call_expr.arg_count > 0) {
        last_arg = ctx.block_statements[node.as.call_expr.first_arg + node.as.call_expr.arg_count - 1];
        if (ctx.nodes[last_arg].type == NodeType::CallExpression || ctx.nodes[last_arg].type == NodeType::Vararg) {
            if (!(is_method_call && node.as.call_expr.arg_count == 1)) {
                last_expands = true;
            }
        }
    }

    out << "([&]() {\n";
    out << "    size_t _ssave_call = L->shadow_top;\n";

    if (is_method_call) {
        out << "    clx::LValue _m_self = ";
        emit_node(ctx.nodes[tgt].as.table_access.table);
        out << ";\n";
        out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_m_self.val, &_m_self.type);\n";

        bool key_is_native = false;
        uint32_t k_idx = ctx.nodes[tgt].as.table_access.key;
        if (yields_number(ctx, state, k_idx, nullptr, state.current_fast_func))
            key_is_native = true;

        out << "    clx::LValue _m_func;\n";
        if (key_is_native) {
            out << "    _m_func = ([&](){ if (_m_self.type != clx::ValueType::Table) "
                   "clx::throw_index_error(L, _m_self); clx::LTable* _t = "
                   "static_cast<clx::LTable*>(_m_self.as_pointer()); "
                   "size_t _k = static_cast<size_t>(";
            emit_native(k_idx);
            out << "); return (_k - 1 < _t->array_size) ? clx::LValue(_t->array[_k - 1], _t->array_types[_k - 1]) : "
                   "clx::table_get_int(L, clx::LValue(clx::ValueType::Table, _t), _k); }());\n";
        } else {
            out << "    _m_func = clx::table_get(L, _m_self, ";
            emit_node(k_idx);
            out << ");\n";
        }
        out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_m_func.val, &_m_func.type);\n";
    }

    if (last_expands) {
        out << "    clx::LValue _dyn_buf[16];\n    size_t _dyn_count = 0;\n";
        if (node.as.call_expr.arg_count > 1) {
            for (uint32_t i = 0; i < node.as.call_expr.arg_count - 1; ++i) {
                if (i > 0)
                    out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_dyn_buf[" << (i - 1)
                        << "].val, &_dyn_buf[" << (i - 1) << "].type);\n";
                out << "    _dyn_buf[_dyn_count++] = ";
                if (is_method_call && i == 0)
                    out << "_m_self";
                else
                    emit_node(ctx.block_statements[node.as.call_expr.first_arg + i]);
                out << ";\n";
            }
        }

        state.expect_multivalue = true;
        out << "    clx::MultiValue _mret = ";
        emit_node(last_arg);
        out << ";\n";
        state.expect_multivalue = false;

        out << "    for (size_t _mi = 0; _mi < _mret.count; ++_mi) _dyn_buf[_dyn_count++] = _mret[_mi];\n";
        out << "    for (size_t _mi = 0; _mi < _dyn_count; ++_mi) L->shadow_stack[L->shadow_top++] = "
               "clx::TypedSlot(&_dyn_buf[_mi].val, &_dyn_buf[_mi].type);\n";

        if (is_direct) {
            out << "    const clx::CFunctionType &_gc_self = " << impl_call(fname) << ";\n";
            out << "    clx::MultiValue _main_ret = clx::call_cfunc_direct(L, _gc_self, _dyn_buf, _dyn_count);\n";
        } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
            && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
            auto _chit = state.hoisted_lookups.find(node.as.call_expr.target);
            if (_chit != state.hoisted_lookups.end()) {
                auto _cf_it = state.hoisted_cfuncs.find(_chit->second);
                if (_cf_it != state.hoisted_cfuncs.end()) {
                    out << "    clx::MultiValue _main_ret = clx::call_cfunc_rooted(L, clx::" << _cf_it->second
                        << ", _dyn_buf, _dyn_count);\n";
                } else {
                    goto _call_direct_dyn;
                }
            } else {
                goto _call_direct_dyn;
            }
        } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
            && ctx.nodes[node.as.call_expr.target].type == NodeType::Identifier
            && !ctx.nodes[node.as.call_expr.target].as.ident.is_global) {
            std::string_view _alias_nm(
                ctx.nodes[node.as.call_expr.target].as.ident.name, ctx.nodes[node.as.call_expr.target].as.ident.length);
            auto _alias_it = state.builtin_aliases.find(std::string(_alias_nm));
            if (_alias_it != state.builtin_aliases.end() && state.reassigned_vars.count(std::string(_alias_nm)) == 0) {
                out << "    clx::MultiValue _main_ret = clx::call_cfunc_rooted(L, clx::" << _alias_it->second
                    << ", _dyn_buf, _dyn_count);\n";
            } else {
                goto _call_direct_dyn;
            }
        } else {
        _call_direct_dyn:;
            out << "    clx::MultiValue _main_ret = clx::call_direct_rooted(L, ";
            if (is_method_call)
                out << "_m_func";
            else
                emit_node(node.as.call_expr.target);
            out << ", _dyn_buf, _dyn_count, \"" << ctx.filename << "\", " << node.line << ");\n";
        }

        out << "    L->shadow_top = _ssave_call;\n";

        if (want_multi)
            out << "    return _main_ret;\n";
        else
            out << "    return (_main_ret.count > 0) ? _main_ret[0] : clx::LValue();\n";
    } else {
        std::set<std::string_view> used_sb;
        for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i) {
            uint32_t av = ctx.block_statements[node.as.call_expr.first_arg + i];
            collect_string_builder_refs(ctx, av, state.string_builders, used_sb);
        }
        for (const auto &sb_name : used_sb) {
            bool has_enclosing_builder = false;
            for (auto _it = locals.rbegin(); _it != locals.rend(); ++_it) {
                if (_it->name == sb_name) {
                    has_enclosing_builder = _it->has_sb;
                    break;
                }
            }
            if (has_enclosing_builder)
                continue;
            if (!state.global_string_builders.count(sb_name) && !state.module_string_builders.count(sb_name)) {
                out << "    clx::StringBuilder sb_" << sb_name << ";\n";
            }
        }
        if (node.as.call_expr.arg_count > 0) {
            out << "    clx::LValue args[" << node.as.call_expr.arg_count << "];\n";
            for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i) {
                if (i > 0)
                    out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[" << (i - 1) << "].val, &args["
                        << (i - 1) << "].type);\n";
                out << "    args[" << i << "] = ";
                if (is_method_call && i == 0)
                    out << "_m_self";
                else
                    emit_node(ctx.block_statements[node.as.call_expr.first_arg + i]);
                out << ";\n";
            }
            out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[" << (node.as.call_expr.arg_count - 1)
                << "].val, &args[" << (node.as.call_expr.arg_count - 1) << "].type);\n";
            bool _sub1 = false;
            bool _subvar = false;
            int64_t _subi = 0;
            if (!is_method_call && node.as.call_expr.arg_count >= 3 && node.as.call_expr.target < ctx.nodes.size()
                && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess
                && state.reassigned_vars.count("string") == 0
                && node.as.call_expr.first_arg + 2 < ctx.block_statements.size()) {
                uint32_t _stt = ctx.nodes[node.as.call_expr.target].as.table_access.table;
                uint32_t _stk = ctx.nodes[node.as.call_expr.target].as.table_access.key;
                if (_stt < ctx.nodes.size() && _stk < ctx.nodes.size() && ctx.nodes[_stt].type == NodeType::Identifier
                    && ctx.nodes[_stk].type == NodeType::String && ctx.nodes[_stt].as.ident.is_global
                    && std::string_view(ctx.nodes[_stt].as.ident.name, ctx.nodes[_stt].as.ident.length) == "string"
                    && std::string_view(ctx.nodes[_stk].as.string.text, ctx.nodes[_stk].as.string.length) == "sub") {
                    uint32_t _snid = ctx.block_statements[node.as.call_expr.first_arg + 1];
                    uint32_t _enid = ctx.block_statements[node.as.call_expr.first_arg + 2];
                    if (_snid < ctx.nodes.size() && _enid < ctx.nodes.size()) {
                        auto &_sns = ctx.nodes[_snid];
                        auto &_ens = ctx.nodes[_enid];
                        if (_sns.type == NodeType::Integer && _ens.type == NodeType::Integer
                            && _sns.as.integer.val == _ens.as.integer.val && _sns.as.integer.val >= 1) {
                            _sub1 = true;
                            _subi = _sns.as.integer.val;
                        } else if (_sns.type == NodeType::Number && _ens.type == NodeType::Number
                            && _sns.as.number.val == _ens.as.number.val
                            && static_cast<double>(static_cast<int64_t>(_sns.as.number.val)) == _sns.as.number.val
                            && _sns.as.number.val >= 1.0) {
                            _sub1 = true;
                            _subi = static_cast<int64_t>(_sns.as.number.val);
                        } else if (_sns.type == NodeType::Identifier && _ens.type == NodeType::Identifier
                            && !_sns.as.ident.is_global && !_ens.as.ident.is_global
                            && std::string_view(_sns.as.ident.name, _sns.as.ident.length)
                                == std::string_view(_ens.as.ident.name, _ens.as.ident.length)) {
                            _sub1 = true;
                            _subvar = true;
                        }
                    }
                }
            }
            if (_sub1) {
                out << "    size_t _ssaved = L->shadow_top;\n";
                out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count
                    << "; ++i) L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[i].val, &args[i].type);\n";
                out << "    clx::MultiValue _main_ret;\n";
                if (_subvar) {
                    out << "    int64_t _sk = 0;\n";
                    out << "    bool _sok = clx::to_integer(args[1], _sk);\n";
                    out << "    if (!_sok) { double _sd = 0.0; if (args[1].to_number(_sd)) { _sk = "
                           "static_cast<int64_t>(_sd); _sok = true; } }\n";
                } else {
                    out << "    int64_t _sk = " << _subi << ";\n";
                    out << "    bool _sok = true;\n";
                }
                out << "    if (_sok && args[0].type == clx::ValueType::String) {\n";
                out << "        size_t _sl = args[0].string_len();\n";
                out << "        if (_sk >= 1 && static_cast<size_t>(_sk) <= _sl) {\n";
                out << "            _main_ret = clx::MultiValue(clx::make_string_pooled(L, args[0].as_string() + "
                    << "static_cast<size_t>(_sk) - 1, 1));\n";
                out << "        } else {\n";
                out << "            _main_ret = clx::MultiValue(clx::LValue(L->intern_string(\"\", 0)));\n";
                out << "        }\n";
                out << "    } else {\n";
                out << "        _main_ret = clx::str_sub(L, args, " << node.as.call_expr.arg_count << ");\n";
                out << "    }\n";
                out << "    L->shadow_top = _ssaved;\n";
            } else if (is_direct) {
                out << "    const clx::CFunctionType &_gc_self = " << impl_call(fname) << ";\n";
                out << "    clx::MultiValue _main_ret = clx::call_cfunc_direct(L, _gc_self, args, "
                    << node.as.call_expr.arg_count << ");\n";
            } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
                && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
                auto _chit = state.hoisted_lookups.find(node.as.call_expr.target);
                if (_chit != state.hoisted_lookups.end()) {
                    auto _cf_it = state.hoisted_cfuncs.find(_chit->second);
                    if (_cf_it != state.hoisted_cfuncs.end()) {
                        out << "    clx::MultiValue _main_ret = clx::call_cfunc_rooted(L, clx::" << _cf_it->second
                            << ", args, " << node.as.call_expr.arg_count << ");\n";
                    } else {
                        goto _call_direct_normal;
                    }
                } else {
                    goto _call_direct_normal;
                }
            } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
                && ctx.nodes[node.as.call_expr.target].type == NodeType::Identifier
                && !ctx.nodes[node.as.call_expr.target].as.ident.is_global) {
                std::string_view _alias_nm(ctx.nodes[node.as.call_expr.target].as.ident.name,
                    ctx.nodes[node.as.call_expr.target].as.ident.length);
                auto _alias_it = state.builtin_aliases.find(std::string(_alias_nm));
                if (_alias_it != state.builtin_aliases.end()
                    && state.reassigned_vars.count(std::string(_alias_nm)) == 0) {
                    out << "    clx::MultiValue _main_ret = clx::call_cfunc_rooted(L, clx::" << _alias_it->second
                        << ", args, " << node.as.call_expr.arg_count << ");\n";
                } else {
                    goto _call_direct_normal;
                }
            } else {
            _call_direct_normal:
                out << "    clx::MultiValue _main_ret = clx::call_direct_rooted(L, ";
                if (is_method_call)
                    out << "_m_func";
                else
                    emit_node(node.as.call_expr.target);
                out << ", args, " << node.as.call_expr.arg_count << ", \"" << ctx.filename << "\", " << node.line
                    << ");\n";
            }
        } else {
            if (is_direct) {
                out << "    const clx::CFunctionType &_gc_self = " << impl_call(fname) << ";\n";
                out << "    clx::MultiValue _main_ret = _gc_self(L, nullptr, 0);\n";
            } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
                && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
                auto _chit = state.hoisted_lookups.find(node.as.call_expr.target);
                if (_chit != state.hoisted_lookups.end()) {
                    auto _cf_it = state.hoisted_cfuncs.find(_chit->second);
                    if (_cf_it != state.hoisted_cfuncs.end())
                        out << "    clx::MultiValue _main_ret = clx::" << _cf_it->second << "(L, nullptr, 0);\n";
                    else {
                        goto _call_direct_normal2;
                    }
                } else {
                    goto _call_direct_normal2;
                }
            } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
                && ctx.nodes[node.as.call_expr.target].type == NodeType::Identifier
                && !ctx.nodes[node.as.call_expr.target].as.ident.is_global) {
                std::string_view _alias_nm(ctx.nodes[node.as.call_expr.target].as.ident.name,
                    ctx.nodes[node.as.call_expr.target].as.ident.length);
                auto _alias_it = state.builtin_aliases.find(std::string(_alias_nm));
                if (_alias_it != state.builtin_aliases.end()
                    && state.reassigned_vars.count(std::string(_alias_nm)) == 0)
                    out << "    clx::MultiValue _main_ret = clx::" << _alias_it->second << "(L, nullptr, 0);\n";
                else {
                    goto _call_direct_normal2;
                }
            } else {
            _call_direct_normal2:;
                out << "    clx::MultiValue _main_ret = clx::call_direct(L, ";
                if (is_method_call)
                    out << "_m_func";
                else
                    emit_node(node.as.call_expr.target);
                out << ", nullptr, 0, \"" << ctx.filename << "\", " << node.line << ");\n";
            }
        }

        out << "    L->shadow_top = _ssave_call;\n";

        if (want_multi)
            out << "    return _main_ret;\n";
        else
            out << "    return (_main_ret.count > 0) ? _main_ret[0] : clx::LValue();\n";
    }
    out << "}())";
}

//------------------ emitParenExpression: handles NodeType::ParenExpression
void CodeEmitter::emitParenExpression(const ASTNode &node, uint32_t node_idx) {
    bool want_multi = state.expect_multivalue;
    state.expect_multivalue = false;

    if (want_multi) {
        out << "clx::MultiValue(";
        emit_node(node.as.paren_expr.expr);
        out << ")";
    } else {
        emit_node(node.as.paren_expr.expr);
    }
    state.expect_multivalue = want_multi;
}

//------------------ native call helpers (see codegen.h)
bool CodeEmitter::native_call_eligible(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count) {
    if (target_idx >= ctx.nodes.size() || ctx.nodes[target_idx].type != NodeType::Identifier
        || ctx.nodes[target_idx].as.ident.is_global)
        return false;
    std::string_view fname(ctx.nodes[target_idx].as.ident.name, ctx.nodes[target_idx].as.ident.length);
    auto npc = state.func_param_counts.find(fname);
    if (state.native_emitted.count(fname) == 0 || npc == state.func_param_counts.end() || arg_count != npc->second)
        return false;
    if (arg_count > 0) {
        uint32_t last = ctx.block_statements[first_arg + arg_count - 1];
        if (last < ctx.nodes.size()
            && (ctx.nodes[last].type == NodeType::CallExpression || ctx.nodes[last].type == NodeType::Vararg))
            return false;
    }
    return true;
}

bool CodeEmitter::try_emit_native_call(uint32_t target_idx, uint32_t first_arg, uint32_t arg_count, bool wrap_multi) {
    if (!native_call_eligible(target_idx, first_arg, arg_count))
        return false;
    std::string_view fname(ctx.nodes[target_idx].as.ident.name, ctx.nodes[target_idx].as.ident.length);
    if (wrap_multi)
        out << "clx::MultiValue(";
    if (state.in_native_impl && fname == state.current_native_func)
        out << "self(self, L";
    else
        out << "native_" << fname << "(L";
    for (uint32_t i = 0; i < arg_count; ++i) {
        out << ", ";
        emit_node(ctx.block_statements[first_arg + i]);
    }
    out << ")";
    if (wrap_multi)
        out << ")";
    return true;
}

//------------------ emitUnaryOp: handles NodeType::UnaryOp
void CodeEmitter::emitUnaryOp(const ASTNode &node, uint32_t node_idx) {
    if (node.as.unary_op.op == static_cast<int>(UnaryOp::Len)) {

        std::string_view _len_tname;
        if (ctx.nodes[node.as.unary_op.expr].type == NodeType::Identifier) {
            _len_tname = std::string_view(
                ctx.nodes[node.as.unary_op.expr].as.ident.name, ctx.nodes[node.as.unary_op.expr].as.ident.length);
        }
        if (!_len_tname.empty() && state.pure_numeric_arrays.count(_len_tname)) {
            out << "clx::LValue(static_cast<int64_t>(l_" << _len_tname << ".size()))";
        } else if (!_len_tname.empty() && !state.tables_with_dynamic_length.count(_len_tname)) {
            auto _klit = state.known_table_lengths.find(_len_tname);
            if (_klit != state.known_table_lengths.end()) {
                out << "clx::LValue(static_cast<int64_t>(" << _klit->second << "))";
            } else {
                out << "clx::len(L, ";
                emit_node(node.as.unary_op.expr);
                out << ")";
            }
        } else {
            out << "clx::len(L, ";
            emit_node(node.as.unary_op.expr);
            out << ")";
        }
    }
    if (node.as.unary_op.op == static_cast<int>(UnaryOp::Minus)) {
        if (yields_number(ctx, state, node.as.unary_op.expr, nullptr, state.current_fast_func)) {
            if (ctx.nodes[node.as.unary_op.expr].type == NodeType::Integer) {
                int64_t iv = ctx.nodes[node.as.unary_op.expr].as.integer.val;
                out << "clx::integer(static_cast<int64_t>(-(" << iv << "ll)))";
            } else {
                if (clx::is_purely_integer_expr(ctx, state, node.as.unary_op.expr)) {
                    out << "clx::LValue(static_cast<int64_t>(-(";
                    emit_native(node.as.unary_op.expr);
                    out << ")))";
                } else {
                    std::string ef = int_flag_expr(node.as.unary_op.expr, 1);
                    if (ef != "false") {
                        out << "(" << ef << ") ? clx::LValue(clx::int_neg(" << int_value_expr(node.as.unary_op.expr, 1)
                            << ")) : clx::LValue(-(";
                        emit_native(node.as.unary_op.expr);
                        out << "))";
                    } else {
                        out << "clx::LValue(static_cast<double>(-(";
                        emit_native(node.as.unary_op.expr);
                        out << ")))";
                    }
                }
            }
        } else {
            out << "clx::unm(L, ";
            emit_node(node.as.unary_op.expr);
            out << ")";
        }
    }
    if (node.as.unary_op.op == static_cast<int>(UnaryOp::BNot)) {
        if (clx::is_purely_integer_expr(ctx, state, node.as.unary_op.expr)) {
            out << "clx::LValue(static_cast<int64_t>(~static_cast<int64_t>(";
            emit_native(node.as.unary_op.expr);
            out << ")))";
        } else {
            out << "clx::bnot(L, ";
            emit_node(node.as.unary_op.expr);
            out << ")";
        }
    }
    if (node.as.unary_op.op == static_cast<int>(UnaryOp::Not)) {
        out << "clx::logical_not(";
        emit_node(node.as.unary_op.expr);
        out << ")";
    }
}

//------------------ try_emit_array_cmp: numeric array fast path for two hoisted integer-indexed reads
bool CodeEmitter::try_emit_array_cmp(int cmp_op, uint32_t l_idx, uint32_t r_idx) {
    if (l_idx == 0xFFFFFFFF || r_idx == 0xFFFFFFFF || l_idx >= ctx.nodes.size() || r_idx >= ctx.nodes.size())
        return false;
    const ASTNode &ln = ctx.nodes[l_idx];
    const ASTNode &rn = ctx.nodes[r_idx];
    if (ln.type != NodeType::TableAccess || rn.type != NodeType::TableAccess)
        return false;
    uint32_t lb = ln.as.table_access.table;
    uint32_t rb = rn.as.table_access.table;
    uint32_t lk = ln.as.table_access.key;
    uint32_t rk = rn.as.table_access.key;
    if (lb >= ctx.nodes.size() || rb >= ctx.nodes.size() || lk >= ctx.nodes.size() || rk >= ctx.nodes.size())
        return false;
    if (ctx.nodes[lb].type != NodeType::Identifier || ctx.nodes[rb].type != NodeType::Identifier)
        return false;
    if (!is_purely_integer_expr(ctx, state, lk) || !is_purely_integer_expr(ctx, state, rk))
        return false;
    std::string lp = hoisted_table_ptr(lb);
    std::string rp = hoisted_table_ptr(rb);
    if (lp.empty() || rp.empty())
        return false;

    const char *cmp = (cmp_op == static_cast<int>(BinaryOp::Le)) ? "<=" : "<";
    const char *fn = (cmp_op == static_cast<int>(BinaryOp::Le)) ? "le" : "lt";
    out << "([&]() -> clx::LValue { clx::LTable* _ca = " << lp << "; clx::LTable* _cb = " << rp
        << "; int64_t _ka = static_cast<int64_t>(";
    emit_native(lk);
    out << "); int64_t _kb = static_cast<int64_t>(";
    emit_native(rk);
    out << "); if (_ka >= 1 && _kb >= 1 && static_cast<size_t>(_ka) <= _ca->array_size"
           " && static_cast<size_t>(_kb) <= _cb->array_size) [[likely]] {"
           " clx::ValueType _tya = _ca->array_types[_ka - 1]; clx::ValueType _tyb = _cb->array_types[_kb - 1];"
           " if (_tya == clx::ValueType::Int64 && _tyb == clx::ValueType::Int64) [[likely]]"
           " return clx::LValue(_ca->array[_ka - 1].payload.i64 "
        << cmp
        << " _cb->array[_kb - 1].payload.i64);"
           " if ((_tya == clx::ValueType::Int64 || _tya == clx::ValueType::Double)"
           " && (_tyb == clx::ValueType::Int64 || _tyb == clx::ValueType::Double)) return clx::LValue(("
           "_tya == clx::ValueType::Int64 ? static_cast<double>(_ca->array[_ka - 1].payload.i64)"
           " : _ca->array[_ka - 1].payload.f64) "
        << cmp
        << " (_tyb == clx::ValueType::Int64 ? static_cast<double>(_cb->array[_kb - 1].payload.i64)"
           " : _cb->array[_kb - 1].payload.f64)); } return clx::"
        << fn << "(L, ";
    emit_node(l_idx);
    out << ", ";
    emit_node(r_idx);
    out << "); })()";
    return true;
}

//------------------ emitBinaryOp: handles NodeType::BinaryOp
void CodeEmitter::emitBinaryOp(const ASTNode &node, uint32_t node_idx) {
    int op = node.as.bin_op.op;

    bool left_native = yields_number(ctx, state, node.as.bin_op.left, nullptr, state.current_fast_func);
    bool right_native = yields_number(ctx, state, node.as.bin_op.right, nullptr, state.current_fast_func);

    if (left_native && right_native) {
        bool both_int = clx::is_purely_integer_expr(ctx, state, node.as.bin_op.left)
            && clx::is_purely_integer_expr(ctx, state, node.as.bin_op.right);
        if (op == static_cast<int>(BinaryOp::Add) || op == static_cast<int>(BinaryOp::Sub)
            || op == static_cast<int>(BinaryOp::Mul)) {
            if (both_int) {
                out << (op == 1   ? "clx::int_add_lv(static_cast<int64_t>("
                        : op == 2 ? "clx::int_sub_lv(static_cast<int64_t>("
                                  : "clx::int_mul_lv(static_cast<int64_t>(");
                emit_native(node.as.bin_op.left);
                out << "), static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << "))";
            } else if (op == static_cast<int>(BinaryOp::Add) || op == static_cast<int>(BinaryOp::Sub)
                || op == static_cast<int>(BinaryOp::Mul)) {
                std::string lf = int_flag_expr(node.as.bin_op.left, 1);
                std::string rf = int_flag_expr(node.as.bin_op.right, 1);
                if (lf != "false" || rf != "false") {
                    static const char *exact_fn[] = { "", "clx::add_exact", "clx::sub_exact", "clx::mul_exact" };
                    out << exact_fn[op] << "((" << lf << "), ";
                    emit_native(node.as.bin_op.left);
                    out << ", (" << int_value_expr(node.as.bin_op.left, 1) << "), (" << rf << "), ";
                    emit_native(node.as.bin_op.right);
                    out << ", (" << int_value_expr(node.as.bin_op.right, 1) << "))";
                } else {
                    out << "clx::LValue(static_cast<double>(";
                    emit_native(node.as.bin_op.left);
                    out << (op == 1 ? " + " : op == 2 ? " - " : " * ");
                    emit_native(node.as.bin_op.right);
                    out << "))";
                }
            }
            return;
        }
        if (op == static_cast<int>(BinaryOp::Div)) {
            if (is_zero_number_literal(ctx, node.as.bin_op.right)) {
                out << "clx::LValue(static_cast<double>(";
                emit_native(node.as.bin_op.left);
                out << ") * std::numeric_limits<double>::infinity())";
            } else {
                out << "clx::LValue(static_cast<double>(";
                emit_native(node.as.bin_op.left);
                out << ") / static_cast<double>(";
                emit_native(node.as.bin_op.right);
                out << "))";
            }
            return;
        }
        if (op == static_cast<int>(BinaryOp::Mod)) {
            if (both_int) {
                out << "clx::LValue(clx::int_floor_mod(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << "), static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            std::string lf = int_flag_expr(node.as.bin_op.left, 1);
            std::string rf = int_flag_expr(node.as.bin_op.right, 1);
            if (lf != "false" || rf != "false") {
                out << "((" << lf << ") && (" << rf << ")) ? clx::LValue(clx::int_floor_mod("
                    << int_value_expr(node.as.bin_op.left, 1) << ", " << int_value_expr(node.as.bin_op.right, 1)
                    << ")) : clx::LValue(clx::fmod_floor(";
                emit_native(node.as.bin_op.left);
                out << ", ";
                emit_native(node.as.bin_op.right);
                out << "))";
            } else {
                out << "clx::LValue(clx::fmod_floor(";
                emit_native(node.as.bin_op.left);
                out << ", ";
                emit_native(node.as.bin_op.right);
                out << "))";
            }
            return;
        }
        if (op == static_cast<int>(BinaryOp::FloorDiv)) {
            if (both_int) {
                out << "clx::LValue(clx::int_floor_div(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << "), static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            std::string lf = int_flag_expr(node.as.bin_op.left, 1);
            std::string rf = int_flag_expr(node.as.bin_op.right, 1);
            if (lf != "false" || rf != "false") {
                out << "((" << lf << ") && (" << rf << ")) ? clx::LValue(clx::int_floor_div("
                    << int_value_expr(node.as.bin_op.left, 1) << ", " << int_value_expr(node.as.bin_op.right, 1)
                    << ")) : clx::LValue(static_cast<double>(std::floor((";
                emit_native(node.as.bin_op.left);
                out << ") / (";
                emit_native(node.as.bin_op.right);
                out << "))))";
            } else {
                out << "clx::LValue(static_cast<double>(std::floor((";
                emit_native(node.as.bin_op.left);
                out << ") / (";
                emit_native(node.as.bin_op.right);
                out << "))))";
            }
            return;
        }
        if (both_int) {
            if (op == static_cast<int>(BinaryOp::BitAnd)) {
                out << "clx::LValue(static_cast<int64_t>(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << ") & static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            if (op == static_cast<int>(BinaryOp::BitOr)) {
                out << "clx::LValue(static_cast<int64_t>(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << ") | static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            if (op == static_cast<int>(BinaryOp::BitXor)) {
                out << "clx::LValue(static_cast<int64_t>(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << ") ^ static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            if (op == static_cast<int>(BinaryOp::Shl)) {
                out << "clx::LValue(static_cast<int64_t>(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << ") << static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            if (op == static_cast<int>(BinaryOp::Shr)) {
                out << "clx::LValue(static_cast<int64_t>(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << ") >> static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
        }
        if (op >= static_cast<int>(BinaryOp::Eq) && op <= static_cast<int>(BinaryOp::Ne)) {
            static const char *ops[] = { "", "", "", "", "", " == ", " < ", " > ", " <= ", " >= ", " != " };
            if (op >= static_cast<int>(BinaryOp::Eq)) {
                std::string cf = int_flag_expr(node.as.bin_op.left, 1);
                std::string cr = int_flag_expr(node.as.bin_op.right, 1);
                if (cf != "false" && cr != "false") {
                    out << "((" << cf << ") && (" << cr << ")) ? clx::LValue(" << int_value_expr(node.as.bin_op.left, 1)
                        << ops[op] << int_value_expr(node.as.bin_op.right, 1) << ") : clx::LValue(";
                    emit_native(node.as.bin_op.left);
                    out << ops[op];
                    emit_native(node.as.bin_op.right);
                    out << ")";
                    return;
                }
            }
            out << "clx::LValue(";
            emit_native(node.as.bin_op.left);
            out << ops[op];
            emit_native(node.as.bin_op.right);
            out << ")";
            return;
        }
    }

    if (op == static_cast<int>(BinaryOp::Mod)) {
        out << "clx::mod(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::FloorDiv)) {
        out << "clx::idiv(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::Pow)) {
        out << "clx::pow(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::Concat)) {
        std::vector<uint32_t> operands;
        auto collect = [&](auto &self, uint32_t n_idx) -> void {
            while (n_idx != 0xFFFFFFFF && ctx.nodes[n_idx].type == NodeType::ParenExpression) {
                n_idx = ctx.nodes[n_idx].as.paren_expr.expr;
            }
            if (ctx.nodes[n_idx].type == NodeType::BinaryOp
                && ctx.nodes[n_idx].as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                self(self, ctx.nodes[n_idx].as.bin_op.left);
                self(self, ctx.nodes[n_idx].as.bin_op.right);
            } else {
                operands.push_back(n_idx);
            }
        };
        collect(collect, node_idx);

        bool fast_concat = !operands.empty();
        size_t total_str_len = 0;
        for (auto op : operands) {
            if (ctx.nodes[op].type == NodeType::String) {
                total_str_len += ctx.nodes[op].as.string.length;
            } else if (yields_number(ctx, state, op, nullptr, state.current_fast_func)) {
                total_str_len += 32;
            } else {
                fast_concat = false;
                break;
            }
        }
        if (fast_concat) {
            out << "([&](){ char _buf[" << (total_str_len + 1) << "]; char* _p = _buf; ";
            for (auto op : operands) {
                if (ctx.nodes[op].type == NodeType::String) {
                    std::string_view s(ctx.nodes[op].as.string.text, ctx.nodes[op].as.string.length);
                    std::string d(s);
                    out << "clx_memcpy(_p, \"" << cpp_escape(d) << "\", " << d.length() << "); _p += " << d.length()
                        << "; ";
                } else {
                    if (is_integer_typed_expr(ctx, state, op)) {
                        out << "_p += std::snprintf(_p, " << (total_str_len + 1)
                            << " - (_p - _buf), \"%lld\", static_cast<long long>(";
                        emit_native(op);
                        out << ")); ";
                    } else {
                        std::string nf = int_flag_expr(op, 1);
                        if (nf != "false") {
                            out << "_p += clx::clx_format_num(_p, " << (total_str_len + 1) << " - (_p - _buf), ";
                            emit_native(op);
                            out << ", " << nf << "); ";
                        } else {
                            out << "_p += clx::clx_format_double(_p, " << (total_str_len + 1) << " - (_p - _buf), ";
                            emit_native(op);
                            out << "); ";
                        }
                    }
                }
            }
            out << "*_p = '\\0'; return clx::LValue(L->intern_string(_buf, static_cast<size_t>(_p - _buf))); }())";
        } else {
            out << "([&](){\n";
            out << "    size_t _ssave_cat = L->shadow_top;\n";
            out << "    clx::LValue args[" << operands.size() << "];\n";
            for (size_t i = 0; i < operands.size(); ++i) {
                if (i > 0)
                    out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[" << (i - 1) << "].val, &args["
                        << (i - 1) << "].type);\n";
                out << "    args[" << i << "] = ";
                emit_node(operands[i]);
                out << ";\n";
            }
            out << "    clx::LValue _cat_r = clx::concat_multi(L, args, " << operands.size() << ");\n";
            out << "    L->shadow_top = _ssave_cat;\n";
            out << "    return _cat_r;\n";
            out << "}())";
        }
        return;
    }
    if (op == static_cast<int>(BinaryOp::And)) {
        out << "([&](){ size_t _eg_ab = L->shadow_top; clx::LValue _a = ";
        emit_node(node.as.bin_op.left);
        out << "; L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_a.val, &_a.type); auto _eg_r = _a.as_bool() ? (";
        emit_node(node.as.bin_op.right);
        out << ") : _a; L->shadow_top = _eg_ab; return _eg_r; }())";
        return;
    }
    if (op == static_cast<int>(BinaryOp::Or)) {
        out << "([&](){ size_t _eg_ab = L->shadow_top; clx::LValue _a = ";
        emit_node(node.as.bin_op.left);
        out << "; L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_a.val, &_a.type); auto _eg_r = _a.as_bool() ? _a "
               ": (";
        emit_node(node.as.bin_op.right);
        out << "); L->shadow_top = _eg_ab; return _eg_r; }())";
        return;
    }

    if (op == static_cast<int>(BinaryOp::BitAnd)) {
        out << "clx::band(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::BitOr)) {
        out << "clx::bor(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::BitXor)) {
        out << "clx::bxor(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::Shl)) {
        out << "clx::shl(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }
    if (op == static_cast<int>(BinaryOp::Shr)) {
        out << "clx::shr(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }

    if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) {
        static const char *fn[] = { "", "add", "sub", "mul", "div" };
        out << "clx::" << fn[op] << "(L, ";
        emit_node(node.as.bin_op.left);
        out << ", ";
        emit_node(node.as.bin_op.right);
        out << ")";
        return;
    }

    if (op >= static_cast<int>(BinaryOp::Eq) && op <= static_cast<int>(BinaryOp::Ne)) {
        static const char *fn[] = { "", "", "", "", "", "eq", "lt", "lt", "le", "le", "eq" };
        bool swapped = op == static_cast<int>(BinaryOp::Gt) || op == static_cast<int>(BinaryOp::Ge);
        uint32_t ord_l = swapped ? node.as.bin_op.right : node.as.bin_op.left;
        uint32_t ord_r = swapped ? node.as.bin_op.left : node.as.bin_op.right;
        if (op == static_cast<int>(BinaryOp::Lt) || op == static_cast<int>(BinaryOp::Gt)) {
            if (try_emit_array_cmp(static_cast<int>(BinaryOp::Lt), ord_l, ord_r))
                return;
        } else if (op == static_cast<int>(BinaryOp::Le) || op == static_cast<int>(BinaryOp::Ge)) {
            if (try_emit_array_cmp(static_cast<int>(BinaryOp::Le), ord_l, ord_r))
                return;
        }
        auto emit_cmp_operand = [&](uint32_t oi) {
            if (oi < ctx.nodes.size() && ctx.nodes[oi].type == NodeType::Integer)
                out << "clx::Integer(static_cast<int64_t>(" << ctx.nodes[oi].as.integer.val << "))";
            else
                emit_node(oi);
        };
        if (op == static_cast<int>(BinaryOp::Ne))
            out << "clx::LValue(!(";
        out << "clx::" << fn[op] << "(L, ";
        emit_cmp_operand(ord_l);
        out << ", ";
        emit_cmp_operand(ord_r);
        out << ")";
        if (op == static_cast<int>(BinaryOp::Ne))
            out << ").as_bool())";
    } else {
        static const char *op_strings[]
            = { "", " + ", " - ", " * ", " / ", " == ", " < ", " > ", " <= ", " >= ", " != " };
        out << "(";
        emit_node(node.as.bin_op.left);
        out << op_strings[op];
        emit_node(node.as.bin_op.right);
        out << ")";
    }
}

//------------------ emitTrueLiteral: handles NodeType::TrueLiteral
void CodeEmitter::emitTrueLiteral(const ASTNode &node, uint32_t node_idx) {
    out << "clx::LValue(true)";
}

//------------------ emitFalseLiteral: handles NodeType::FalseLiteral
void CodeEmitter::emitFalseLiteral(const ASTNode &node, uint32_t node_idx) {
    out << "clx::LValue(false)";
}

//------------------ emitNilLiteral: handles NodeType::NilLiteral
void CodeEmitter::emitNilLiteral(const ASTNode &node, uint32_t node_idx) {
    out << "clx::LValue()";
}

void CodeEmitter::emitTableOp(int bin_op, uint32_t lhs_tbl, uint32_t lhs_key, uint32_t const_idx, bool nil_ok) {
    const char *fn = nullptr;
    if (bin_op == static_cast<int>(BinaryOp::Add))
        fn = "table_increment";
    else if (bin_op == static_cast<int>(BinaryOp::Sub))
        fn = "table_decrement";
    else if (bin_op == static_cast<int>(BinaryOp::Mul))
        fn = "table_multiply";
    else
        fn = "table_divide";
    out << "clx::" << fn << "(L, ";
    emit_node(lhs_tbl);
    out << ", ";
    emit_node(lhs_key);
    out << ", ";
    emit_native(const_idx);
    out << ", " << (nil_ok ? "true" : "false") << ");\n";
}

//------------------ emitNumber: handles NodeType::Number
void CodeEmitter::emitNumber(const ASTNode &node, uint32_t node_idx) {
    out << "clx::LValue(static_cast<double>(" << node.as.number.val << "))";
}

//------------------ emitInteger: handles NodeType::Integer
void CodeEmitter::emitInteger(const ASTNode &node, uint32_t node_idx) {
    out << "clx::integer(static_cast<int64_t>(" << node.as.integer.val << "))";
}

//------------------ box_native_identifier: boxes a native (raw double) local/param into an LValue
void CodeEmitter::box_native_identifier(std::string_view emit_name, std::string_view lua_name) {
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (it->name == lua_name) {
            if (it->has_intf && it->has_ii) {
                out << "clx::box_int_flag(l_" << emit_name << ", _ii_l_" << emit_name << ", _intf_l_" << emit_name
                    << ")";
            } else if (it->has_intf) {
                out << "clx::num_box(l_" << emit_name << ", _intf_l_" << emit_name << ")";
            } else {
                out << "clx::LValue(l_" << emit_name << ")";
            }
            return;
        }
    }
    out << "clx::LValue(l_" << emit_name << ")";
}

//------------------ emitIdentifier: handles NodeType::Identifier
void CodeEmitter::emitIdentifier(const ASTNode &node, uint32_t node_idx) {
    std::string_view name(node.as.ident.name, node.as.ident.length);
    bool is_native
        = std::find(state.native_numbers.begin(), state.native_numbers.end(), name) != state.native_numbers.end();
    bool is_boxed = false;
    std::string_view cpp_name;
    bool is_loc = this->is_local(name, is_boxed, cpp_name);
    std::string_view emit_name = cpp_name.empty() ? name : cpp_name;

    if (node.as.ident.is_global) {
        if (name == "_ENV") {
            out << "_ENV";
        } else if (state.global_constants.count(name)) {
            out << "clx::LValue(static_cast<double>(" << state.global_constants[name] << "))";
        } else if (state.string_builders.count(name)) {
            out << "(sb_" << emit_name << ".empty() ? clx::get_env_var(L, _ENV, \"" << name << "\") : clx::LValue(sb_"
                << emit_name << ".to_string(L)))";
        } else {
            out << "clx::get_env_var(L, _ENV, \"" << name << "\")";
        }
    } else if (is_native && !is_boxed && !node.as.ident.is_captured) {
        box_native_identifier(emit_name, name);
    } else if (is_loc) {
        if (state.string_builders.count(name)) {
            if (is_boxed) {
                out << "(sb_" << emit_name << ".empty() ? (*l_" << emit_name << ") : clx::LValue(sb_" << emit_name
                    << ".to_string(L)))";
            } else {
                out << "(sb_" << emit_name << ".empty() ? l_" << emit_name << " : clx::LValue(sb_" << emit_name
                    << ".to_string(L)))";
            }
        } else if (is_boxed)
            out << "(*l_" << emit_name << ")";
        else if (is_native)
            box_native_identifier(emit_name, name);
        else
            out << "l_" << emit_name;
    } else {
        size_t idx = state.string_pool_index.at(name);
        out << "clx_gettable_safe(L->_G->gettable(cstr_[" << idx << "]))";
    }
}

//------------------ emitString: handles NodeType::String
void CodeEmitter::emitString(const ASTNode &node, uint32_t node_idx) {
    std::string_view s(node.as.string.text, node.as.string.length);
    size_t idx = state.string_pool_index.at(s);
    out << "cstr_[" << idx << "]";
}

//------------------ emitTableAccess: handles NodeType::TableAccess
void CodeEmitter::emitTableAccess(const ASTNode &node, uint32_t node_idx) {
    {
        auto hit = state.hoisted_lookups.find(node_idx);
        if (hit != state.hoisted_lookups.end()) {
            out << hit->second;
            return;
        }
    }
    std::string_view t_name;
    if (ctx.nodes[node.as.table_access.table].type == NodeType::Identifier) {
        t_name = std::string_view(
            ctx.nodes[node.as.table_access.table].as.ident.name, ctx.nodes[node.as.table_access.table].as.ident.length);
    }
    if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
        if (state.int_numeric_arrays.count(std::string(t_name))) {
            out << "clx::LValue(static_cast<int64_t>(l_" << t_name << "[static_cast<size_t>(";
            emit_native(node.as.table_access.key);
            out << ") - 1]))";
        } else {
            out << "clx::LValue(static_cast<double>(l_" << t_name << "[static_cast<size_t>(";
            emit_native(node.as.table_access.key);
            out << ") - 1]))";
        }
        return;
    }
    std::string _hp = hoisted_table_ptr(node.as.table_access.table);
    if (state.bce_safe_nodes.count(node_idx)) {
        out << "([&](){ ";
        if (!_hp.empty()) {
            out << "clx::LTable* _t" << node_idx << " = " << _hp << ";";
        } else {
            out << "clx::LValue _tb" << node_idx << " = ";
            emit_node(node.as.table_access.table);
            out << "; if (_tb" << node_idx << ".type != clx::ValueType::Table) clx::throw_index_error(L, _tb"
                << node_idx << "); clx::LTable* _t" << node_idx << " = static_cast<clx::LTable*>(_tb" << node_idx
                << ".as_pointer());";
        }
        out << " size_t _k" << node_idx << " = static_cast<size_t>(";
        emit_native(node.as.table_access.key);
        out << "); return (_k" << node_idx << " - 1 < _t" << node_idx << "->array_size) ? clx::LValue(_t" << node_idx
            << "->array[_k" << node_idx << " - 1], _t" << node_idx << "->array_types[_k" << node_idx
            << " - 1]) : clx::table_get_int(L, clx::LValue(clx::ValueType::Table, _t" << node_idx << "), _k" << node_idx
            << "); }())";
    } else {
        bool key_is_native = false;
        uint32_t k_idx = node.as.table_access.key;
        if (yields_number(ctx, state, k_idx, nullptr, state.current_fast_func))
            key_is_native = true;

        if (key_is_native) {
            uint32_t _cb = node.as.table_access.table;
            std::vector<uint32_t> _cks;
            _cks.push_back(k_idx);
            while (ctx.nodes[_cb].type == NodeType::TableAccess
                && yields_number(ctx, state, ctx.nodes[_cb].as.table_access.key, nullptr, state.current_fast_func)) {
                _cks.push_back(ctx.nodes[_cb].as.table_access.key);
                _cb = ctx.nodes[_cb].as.table_access.table;
            }
            if (_cks.size() > 1 && ctx.nodes[_cb].type == NodeType::Identifier) {
                std::reverse(_cks.begin(), _cks.end());
                std::string _hp_base = hoisted_table_ptr(_cb);
                out << "([&](){ ";
                if (!_hp_base.empty()) {
                    out << "clx::LTable* _tc = " << _hp_base << ";";
                } else {
                    out << "clx::LValue _b = ";
                    emit_node(_cb);
                    out << "; if (_b.type != clx::ValueType::Table) clx::throw_index_error(L, _b);";
                    out << " clx::LTable* _tc = static_cast<clx::LTable*>(_b.as_pointer());";
                }
                for (size_t _i = 0; _i < _cks.size() - 1; ++_i) {
                    out << " size_t _k" << _i << " = static_cast<size_t>(";
                    emit_native(_cks[_i]);
                    out << ");";
                    out << " clx::LValue _v" << _i << " = (_k" << _i
                        << " - 1 < _tc->array_size) ? clx::LValue(_tc->array[_k" << _i << " - 1], _tc->array_types[_k"
                        << _i << " - 1]) : clx::table_get_int(L, clx::LValue(clx::ValueType::Table, _tc), _k" << _i
                        << ");";
                    out << " if (_v" << _i << ".type != clx::ValueType::Table) clx::throw_index_error(L, _v" << _i
                        << "); _tc = static_cast<clx::LTable*>(_v" << _i << ".as_pointer());";
                }
                size_t _last = _cks.size() - 1;
                out << " size_t _k" << _last << " = static_cast<size_t>(";
                emit_native(_cks[_last]);
                out << ");";
                out << " return (_k" << _last << " - 1 < _tc->array_size) ? clx::LValue(_tc->array[_k" << _last
                    << " - 1], _tc->array_types[_k" << _last
                    << " - 1]) : clx::table_get_int(L, clx::LValue(clx::ValueType::Table, _tc), _k" << _last
                    << "); }())";
            } else {
                out << "([&](){ ";
                if (!_hp.empty()) {
                    out << "clx::LTable* _t = " << _hp << ";";
                } else {
                    out << "clx::LValue _b = ";
                    emit_node(node.as.table_access.table);
                    out << "; if (_b.type != clx::ValueType::Table) clx::throw_index_error(L, _b);";
                    out << " clx::LTable* _t = static_cast<clx::LTable*>(_b.as_pointer());";
                }
                out << " size_t _k = static_cast<size_t>(";
                emit_native(k_idx);
                out << "); return (_k - 1 < _t->array_size) ? clx::LValue(_t->array[_k - 1], _t->array_types[_k - 1]) "
                       ": clx::table_get_int(L, clx::LValue(clx::ValueType::Table, _t), _k); }())";
            }
        } else {
            uint32_t kt = node.as.table_access.key;
            if (kt < ctx.nodes.size() && ctx.nodes[kt].type == NodeType::String) {
                out << "clx::table_get(L, ";
                emit_node(node.as.table_access.table);
                out << ", cstr_["
                    << (state.string_pool_index.at(
                           std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length)))
                    << "])";
            } else {
                out << "clx::table_get(L, ";
                emit_node(node.as.table_access.table);
                out << ", ";
                emit_node(node.as.table_access.key);
                out << ")";
            }
        }
    }
}

//------------------ emitVararg: handles NodeType::Vararg
void CodeEmitter::emitVararg(const ASTNode &node, uint32_t node_idx) {
    if (state.expect_multivalue)
        out << "clx::MultiValue(_va_args, _va_count, L)";
    else
        out << "(_va_count > 0 ? _va_args[0] : clx::LValue())";
}

}
