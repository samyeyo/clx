// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  loops.cpp · Numeric & generic for loops    │
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

//------------------ CodeEmitter::emit_loop_table_hoists (see codegen.h)
void CodeEmitter::emit_loop_table_hoists(uint32_t body_idx, std::vector<std::string_view> &added) {
    if (body_idx == 0xFFFFFFFF || state.table_typed_locals.empty())
        return;
    std::set<std::string, std::less<>> tbl_set(state.table_typed_locals.begin(), state.table_typed_locals.end());
    std::set<std::string_view> used;
    collect_hoisted_tables(ctx, body_idx, tbl_set, used);
    for (std::string_view nm : used) {
        if (state.hoisted_tables.count(nm))
            continue;
        if (state.pure_numeric_arrays.count(nm) || state.int_numeric_arrays.count(nm))
            continue;
        if (state.hoisted_locals.count(nm))
            continue;
        bool is_boxed = false;
        std::string_view cpp_name;
        if (!is_local(nm, is_boxed, cpp_name) || is_boxed)
            continue;
        std::string emit_name(cpp_name.empty() ? std::string(nm) : std::string(cpp_name));
        out << "clx::LTable* _ht_" << emit_name << " = static_cast<clx::LTable*>(l_" << emit_name
            << ".as_pointer());\n";
        state.hoisted_tables[nm] = "_ht_" + emit_name;
        added.push_back(nm);
    }
}

//------------------ CodeEmitter::restore_loop_table_hoists (see codegen.h)
void CodeEmitter::restore_loop_table_hoists(const std::vector<std::string_view> &added) {
    for (std::string_view nm : added)
        state.hoisted_tables.erase(nm);
}

//------------------ emitForStatement: handles NodeType::ForStatement
void CodeEmitter::emitForStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";

    bool native_for = yields_number(ctx, state, node.as.for_stmt.start_expr, nullptr, state.current_fast_func)
        && yields_number(ctx, state, node.as.for_stmt.limit_expr, nullptr, state.current_fast_func)
        && (node.as.for_stmt.step_expr == 0xFFFFFFFF
            || yields_number(ctx, state, node.as.for_stmt.step_expr, nullptr, state.current_fast_func));

    std::string_view var_name(
        ctx.nodes[node.as.for_stmt.var_ident].as.ident.name, ctx.nodes[node.as.for_stmt.var_ident].as.ident.length);
    bool is_cap = ctx.nodes[node.as.for_stmt.var_ident].as.ident.is_captured;
    bool is_n = !is_cap;

    bool step_is_default = (node.as.for_stmt.step_expr == 0xFFFFFFFF);
    bool step_known_positive = step_is_default;
    bool step_known_negative = false;
    if (!step_is_default) {
        auto &step_node = ctx.nodes[node.as.for_stmt.step_expr];
        if (step_node.type == NodeType::Number) {
            double step_val = step_node.as.number.val;
            step_known_positive = (step_val > 0);
            step_known_negative = (step_val < 0);
        } else if (step_node.type == NodeType::Integer) {
            int64_t step_val = step_node.as.integer.val;
            step_known_positive = (step_val > 0);
            step_known_negative = (step_val < 0);
        }
    }

    //------------------ a negated integer literal (-6) is an integer start too: otherwise the counter became a double
    uint32_t start_lit_idx = node.as.for_stmt.start_expr;
    while (ctx.nodes[start_lit_idx].type == NodeType::ParenExpression)
        start_lit_idx = ctx.nodes[start_lit_idx].as.paren_expr.expr;
    if (ctx.nodes[start_lit_idx].type == NodeType::UnaryOp
        && ctx.nodes[start_lit_idx].as.unary_op.op == static_cast<int>(UnaryOp::Minus)) {
        uint32_t inner = ctx.nodes[start_lit_idx].as.unary_op.expr;
        while (ctx.nodes[inner].type == NodeType::ParenExpression)
            inner = ctx.nodes[inner].as.paren_expr.expr;
        if (ctx.nodes[inner].type == NodeType::Integer)
            start_lit_idx = inner;
    }
    bool start_is_int_literal = ctx.nodes[start_lit_idx].type == NodeType::Integer;
    if (!start_is_int_literal && ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::BinaryOp) {
        auto &bin = ctx.nodes[node.as.for_stmt.start_expr].as.bin_op;
        if (bin.op == static_cast<int>(BinaryOp::Add) || bin.op == static_cast<int>(BinaryOp::Sub)) {
            start_is_int_literal = (ctx.nodes[bin.left].type == NodeType::Integer
                                       && yields_number(ctx, state, bin.right, nullptr, state.current_fast_func))
                || (ctx.nodes[bin.right].type == NodeType::Integer
                    && yields_number(ctx, state, bin.left, nullptr, state.current_fast_func));
        }
    }
    uint32_t step_lit_idx = node.as.for_stmt.step_expr;
    if (!step_is_default) {
        while (ctx.nodes[step_lit_idx].type == NodeType::ParenExpression)
            step_lit_idx = ctx.nodes[step_lit_idx].as.paren_expr.expr;
        if (ctx.nodes[step_lit_idx].type == NodeType::UnaryOp
            && ctx.nodes[step_lit_idx].as.unary_op.op == static_cast<int>(UnaryOp::Minus)) {
            uint32_t inner = ctx.nodes[step_lit_idx].as.unary_op.expr;
            while (ctx.nodes[inner].type == NodeType::ParenExpression)
                inner = ctx.nodes[inner].as.paren_expr.expr;
            if (ctx.nodes[inner].type == NodeType::Integer)
                step_lit_idx = inner;
        }
    }
    //------------------ a negated integer literal step (-1) is still an integer step, so the counter stays an integer
    bool step_is_int_lit = !step_is_default && ctx.nodes[step_lit_idx].type == NodeType::Integer;
    if (step_is_int_lit && ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::UnaryOp) {
        //------------------ the sign of a negated literal decides how the float limit is rounded (ceil descending, floor ascending)
        step_known_negative = ctx.nodes[step_lit_idx].as.integer.val > 0;
        step_known_positive = !step_known_negative;
    }
    bool step_is_zero_lit = !step_is_default
        && ((ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Integer
                && ctx.nodes[node.as.for_stmt.step_expr].as.integer.val == 0)
            || (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Number
                && ctx.nodes[node.as.for_stmt.step_expr].as.number.val == 0.0));
    bool step_is_nonzero_lit = !step_is_default
        && ((ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Integer
                && ctx.nodes[node.as.for_stmt.step_expr].as.integer.val != 0)
            || (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Number
                && ctx.nodes[node.as.for_stmt.step_expr].as.number.val != 0.0));
    bool counter_is_int = native_for && !is_cap && start_is_int_literal && (step_is_default || step_is_int_lit);

    out << "{\n";
    if (!native_for)
        out << "clx::FrameGuard _fg_for_" << node_idx << "(_fr);\n";

    if (step_is_zero_lit) {
        out << "{\n";
        out << "throw clx::LRuntimeException(clx::LValue(L->intern_string(std::string(\"" << ctx.filename << ":"
            << node.line << ": 'for' step is zero\"))));\n";
        out << "}\n";
    } else if (native_for) {
        if (counter_is_int) {
            auto &start_node = ctx.nodes[node.as.for_stmt.start_expr];
            if (start_node.type == NodeType::BinaryOp) {
                auto &bin = start_node.as.bin_op;
                out << "int64_t s_" << node_idx << " = static_cast<int64_t>(";
                emit_native(bin.left);
                out << ") " << (bin.op == static_cast<int>(BinaryOp::Add) ? "+" : "-") << " static_cast<int64_t>(";
                emit_native(bin.right);
                out << ");\n";

            } else {
                out << "int64_t s_" << node_idx << " = ";
                emit_native(node.as.for_stmt.start_expr);
                out << ";\n";

            }
            if (ctx.nodes[node.as.for_stmt.limit_expr].type == NodeType::Integer) {
                out << "int64_t l_" << node_idx << " = ";
                emit_native(node.as.for_stmt.limit_expr);
                out << ";\n";

            } else if (step_known_negative) {
                out << "int64_t l_" << node_idx << " = static_cast<int64_t>(std::ceil(";
                emit_native(node.as.for_stmt.limit_expr);
                out << "));\n";

            } else {
                out << "int64_t l_" << node_idx << " = static_cast<int64_t>(std::floor(";
                emit_native(node.as.for_stmt.limit_expr);
                out << "));\n";

            }
            if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
                out << "int64_t st_" << node_idx << " = ";
                emit_native(node.as.for_stmt.step_expr);
                out << ";\n";

            } else {
                out << "int64_t st_" << node_idx << " = 1;\n";
            }
        } else {
                out << "double s_" << node_idx << " = ";
                emit_native(node.as.for_stmt.start_expr);
                out << ";\n";

                out << "double l_" << node_idx << " = ";
                emit_native(node.as.for_stmt.limit_expr);
                out << ";\n";

            if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
                out << "double st_" << node_idx << " = ";
                emit_native(node.as.for_stmt.step_expr);
                out << ";\n";

            } else {
                out << "double st_" << node_idx << " = 1.0;\n";
            }
            if (!step_is_default && !step_is_nonzero_lit) {
                out << "if (st_" << node_idx << " == 0.0)\n";
                out << "throw clx::LRuntimeException(clx::LValue(L->intern_string(std::string(\"" << ctx.filename
                    << ":" << node.line << ": 'for' step is zero\"))));\n";
            }
        }
    } else {
        out << "clx::LValue start_" << node_idx << " = ";
        emit_node(node.as.for_stmt.start_expr);
        out << ";\n";

        out << "_fr.push(start_" << node_idx << ");\n";
        out << "clx::LValue limit_" << node_idx << " = ";
        emit_node(node.as.for_stmt.limit_expr);
        out << ";\n";

        out << "_fr.push(limit_" << node_idx << ");\n";
        if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
            out << "clx::LValue step_" << node_idx << " = ";
            emit_node(node.as.for_stmt.step_expr);
            out << ";\n";

        } else {
            out << "clx::LValue step_" << node_idx << " = clx::LValue(static_cast<int64_t>(1));\n";
        }
        out << "_fr.push(step_" << node_idx << ");\n";
        out << "bool _isint_" << node_idx << " = (start_" << node_idx
            << ".type == clx::ValueType::Int64 && step_" << node_idx << ".type == clx::ValueType::Int64);\n";
        out << "bool _skip_" << node_idx << " = false;\n";
        out << "int64_t is_" << node_idx << " = 0, il_" << node_idx << " = 0, ist_" << node_idx << " = 0;\n";
        out << "double fs_" << node_idx << " = 0, fl_" << node_idx << " = 0, fst_" << node_idx << " = 0;\n";
        out << "if (_isint_" << node_idx << ") {\n";
        out << "is_" << node_idx << " = start_" << node_idx << ".as_integer(); ist_" << node_idx
            << " = step_" << node_idx << ".as_integer();\n";
        out << "if (ist_" << node_idx << " == 0)\n";
        out << "throw clx::LRuntimeException(clx::LValue(L->intern_string(std::string(\"" << ctx.filename << ":"
            << node.line << ": 'for' step is zero\"))));\n";
        out << "_skip_" << node_idx << " = clx::forlimit(L, is_" << node_idx << ", limit_" << node_idx << ", ist_"
            << node_idx << ", il_" << node_idx << ");\n";
        out << "} else {\n";
        out << "if (!limit_" << node_idx << ".to_number(fl_" << node_idx << ")) clx::forerror(L, limit_" << node_idx
            << ", \"limit\");\n";
        out << "if (!step_" << node_idx << ".to_number(fst_" << node_idx << ")) clx::forerror(L, step_" << node_idx
            << ", \"step\");\n";
        out << "if (!start_" << node_idx << ".to_number(fs_" << node_idx << ")) clx::forerror(L, start_" << node_idx
            << ", \"initial value\");\n";
        out << "if (fst_" << node_idx << " == 0)\n";
        out << "throw clx::LRuntimeException(clx::LValue(L->intern_string(std::string(\"" << ctx.filename << ":"
            << node.line << ": 'for' step is zero\"))));\n";
        out << "_skip_" << node_idx << " = (fst_" << node_idx << " > 0) ? (fl_" << node_idx << " < fs_" << node_idx
            << ") : (fs_" << node_idx << " < fl_" << node_idx << ");\n";
        out << "}\n";
    }

    auto _saved_hoisted = std::move(state.hoisted_lookups);
    auto _saved_hoisted_cf = std::move(state.hoisted_cfuncs);
    state.hoisted_lookups.clear();
    state.hoisted_cfuncs.clear();
    std::vector<uint32_t> _invariant_lookups;
    auto _find_invariant = [&](auto &self, uint32_t bn_idx) -> void {
        if (bn_idx == 0xFFFFFFFF || bn_idx >= ctx.nodes.size())
            return;
        auto &bn = ctx.nodes[bn_idx];
        if (bn.type == NodeType::TableAccess) {
            uint32_t tbl = bn.as.table_access.table;
            uint32_t key = bn.as.table_access.key;
            //------------------ only a known builtin function field is hoisted: any other global field can be reassigned by the body
            if (tbl < ctx.nodes.size() && key < ctx.nodes.size() && ctx.nodes[tbl].type == NodeType::Identifier
                && ctx.nodes[tbl].as.ident.is_global && ctx.nodes[key].type == NodeType::String) {
                std::string_view _m(ctx.nodes[tbl].as.ident.name, ctx.nodes[tbl].as.ident.length);
                std::string_view _f(ctx.nodes[key].as.string.text, ctx.nodes[key].as.string.length);
                if (builtin_field_ok(state, _m, _f) && lookup_builtin(_m, _f))
                    _invariant_lookups.push_back(bn_idx);
            }
            self(self, tbl);
        } else if (bn.type == NodeType::Block) {
            for (uint32_t bi = 0; bi < bn.as.block.count; ++bi)
                self(self, ctx.block_statements[bn.as.block.first_statement + bi]);
        } else if (bn.type == NodeType::IfStatement) {
            self(self, bn.as.if_stmt.condition);
            self(self, bn.as.if_stmt.then_block);
            if (bn.as.if_stmt.else_block != 0xFFFFFFFF)
                self(self, bn.as.if_stmt.else_block);
        } else if (bn.type == NodeType::WhileStatement || bn.type == NodeType::RepeatStatement) {
            self(self, bn.as.while_stmt.condition);
            self(self, bn.as.while_stmt.body_block);
        } else if (bn.type == NodeType::ForStatement || bn.type == NodeType::GenericForStatement) {
            self(self, bn.as.for_stmt.start_expr);
            self(self, bn.as.for_stmt.limit_expr);
            self(self, bn.as.for_stmt.step_expr);
            self(self, bn.as.for_stmt.body_block);
        } else if (bn.type == NodeType::DoStatement) {
            self(self, bn.as.do_stmt.body_block);
        } else if (bn.type == NodeType::Assignment) {
            for (uint32_t bi = 0; bi < bn.as.assign.target_count; ++bi)
                self(self, ctx.block_statements[bn.as.assign.first_target + bi]);
            for (uint32_t bi = 0; bi < bn.as.assign.value_count; ++bi)
                self(self, ctx.block_statements[bn.as.assign.first_value + bi]);
        } else if (bn.type == NodeType::LocalDecl || bn.type == NodeType::GlobalDeclStatement) {
            for (uint32_t bi = 0; bi < bn.as.local_decl.value_count; ++bi)
                self(self, ctx.block_statements[bn.as.local_decl.first_value + bi]);
        } else if (bn.type == NodeType::CallExpression) {
            self(self, bn.as.call_expr.target);
            for (uint32_t bi = 0; bi < bn.as.call_expr.arg_count; ++bi)
                self(self, ctx.block_statements[bn.as.call_expr.first_arg + bi]);
        } else if (bn.type == NodeType::BinaryOp) {
            self(self, bn.as.bin_op.left);
            self(self, bn.as.bin_op.right);
        } else if (bn.type == NodeType::UnaryOp) {
            self(self, bn.as.unary_op.expr);
        } else if (bn.type == NodeType::IntrinsicCall) {
            for (uint32_t bi = 0; bi < bn.as.intrinsic_call.arg_count; ++bi)
                self(self, ctx.block_statements[bn.as.intrinsic_call.first_arg + bi]);
        } else if (bn.type == NodeType::ReturnStatement) {
            for (uint32_t bi = 0; bi < bn.as.return_stmt.value_count; ++bi)
                self(self, ctx.block_statements[bn.as.return_stmt.first_value + bi]);
        } else if (bn.type == NodeType::ParenExpression) {
            self(self, bn.as.paren_expr.expr);
        } else if (bn.type == NodeType::TableConstructor) {
            for (uint32_t bi = 0; bi < bn.as.table_cons.count; ++bi) {
                self(self, ctx.block_statements[bn.as.table_cons.first_item + bi * 2]);
                self(self, ctx.block_statements[bn.as.table_cons.first_item + bi * 2 + 1]);
            }
        } else if (bn.type == NodeType::FunctionDef) {
            self(self, bn.as.func_def.body_block);
        }
    };
    _find_invariant(_find_invariant, node.as.for_stmt.body_block);

    std::sort(_invariant_lookups.begin(), _invariant_lookups.end());
    _invariant_lookups.erase(
        std::unique(_invariant_lookups.begin(), _invariant_lookups.end()), _invariant_lookups.end());
    for (uint32_t _h_node : _invariant_lookups) {
        std::string _h_name = "_hoist_" + std::to_string(_h_node);
        out << "clx::LValue " << _h_name << " = clx::table_get(L, ";
        emit_node(ctx.nodes[_h_node].as.table_access.table);
        out << ", ";
        emit_node(ctx.nodes[_h_node].as.table_access.key);
        out << ");\n";
        out << "_fr.push(" << _h_name << ");\n";
        state.hoisted_lookups[_h_node] = _h_name;
        uint32_t _ht = ctx.nodes[_h_node].as.table_access.table;
        uint32_t _hk = ctx.nodes[_h_node].as.table_access.key;
        if (_ht < ctx.nodes.size() && _hk < ctx.nodes.size() && ctx.nodes[_ht].type == NodeType::Identifier
            && ctx.nodes[_ht].as.ident.is_global && ctx.nodes[_hk].type == NodeType::String) {
            std::string_view _hm(ctx.nodes[_ht].as.ident.name, ctx.nodes[_ht].as.ident.length);
            std::string_view _hf(ctx.nodes[_hk].as.string.text, ctx.nodes[_hk].as.string.length);
            //------------------ hoisted cfunc bypasses the captured value, so a written builtin field stays on generic dispatch
            if (builtin_field_ok(state, _hm, _hf)) {
                const char *_cf = lookup_builtin(_hm, _hf);
                if (_cf)
                    state.hoisted_cfuncs[_h_name] = _cf;
            }
        }
    }

    std::vector<std::string_view> _added_ht;
    emit_loop_table_hoists(node.as.for_stmt.body_block, _added_ht);

    auto emit_for_body = [&](bool body_is_int) {
        size_t _nn_sz = state.native_numbers.size();
        bool _had_ni = state.native_integers.count(std::string(var_name)) > 0;
        if (is_cap || !is_n) {
            out << "clx::FrameGuard _fg_iter(_fr);\n";
        }

        if (is_cap) {
            out << "clx::LUpValue l_" << var_name << ";\n";
            out << "l_" << var_name
                << " = clx::make_upvalue((i_val == static_cast<int64_t>(i_val)) ? "
                   "clx::LValue(static_cast<int64_t>(i_val)) : clx::LValue(i_val));\n";
            out << "_fr.push(*l_" << var_name << ");\n";
        } else if (is_n) {
            if (body_is_int) {
                out << "const int64_t l_" << var_name << " = i_val;\n";
                state.native_integers.insert(std::string(var_name));
                state.native_numbers.push_back(var_name);
            } else {
                out << "const double l_" << var_name << " = i_val;\n";
                state.native_integers.erase(std::string(var_name));
                state.native_numbers.push_back(var_name);
            }
        } else {
            out << "const clx::LValue l_" << var_name
                << "( (i_val == static_cast<int64_t>(i_val)) ? clx::LValue(static_cast<int64_t>(i_val)) : "
                   "clx::LValue(i_val) );\n";
        }

        size_t prev_locals = locals.size();
        locals.push_back({ var_name, is_cap });
        locals.back().is_int_counter = body_is_int;
        if (node.as.for_stmt.body_block != 0xFFFFFFFF) {
            bool prev_skip = state.skip_block_braces;
            state.skip_block_braces = true;
            if (state.in_fast_function && node.as.for_stmt.body_block != 0xFFFFFFFF) {
                bool _body_has_goto = false;
                auto _check_goto = [&](auto &self, uint32_t n_idx) -> void {
                    if (n_idx == 0xFFFFFFFF || n_idx >= ctx.nodes.size() || _body_has_goto)
                        return;
                    auto &_n = ctx.nodes[n_idx];
                    if (_n.type == NodeType::GotoStatement) {
                        _body_has_goto = true;
                        return;
                    }
                    if (_n.type == NodeType::Block) {
                        for (uint32_t _bi = 0; _bi < _n.as.block.count && !_body_has_goto; ++_bi)
                            self(self, ctx.block_statements[_n.as.block.first_statement + _bi]);
                    } else if (_n.type == NodeType::IfStatement) {
                        self(self, _n.as.if_stmt.then_block);
                        if (_n.as.if_stmt.else_block != 0xFFFFFFFF)
                            self(self, _n.as.if_stmt.else_block);
                    } else if (_n.type == NodeType::WhileStatement) {
                        self(self, _n.as.while_stmt.body_block);
                    } else if (_n.type == NodeType::RepeatStatement) {
                        self(self, _n.as.repeat_stmt.body_block);
                    } else if (_n.type == NodeType::ForStatement || _n.type == NodeType::GenericForStatement) {
                        self(self, _n.as.for_stmt.body_block);
                    } else if (_n.type == NodeType::DoStatement) {
                        self(self, _n.as.do_stmt.body_block);
                    }
                };
                _check_goto(_check_goto, node.as.for_stmt.body_block);
                if (_body_has_goto)
                    out << "std::atomic_signal_fence(std::memory_order_seq_cst);\n";
            }
            emit_node(node.as.for_stmt.body_block);
            state.skip_block_braces = prev_skip;
        }
        locals.resize(prev_locals);
        state.native_numbers.resize(_nn_sz);
        if (!_had_ni)
            state.native_integers.erase(std::string(var_name));
    };

    if (step_is_zero_lit) {
        //------------------ step-zero throw already emitted above; no loop to emit
    } else if (native_for) {
        if (counter_is_int) {
            auto &start_node = ctx.nodes[node.as.for_stmt.start_expr];
            bool start_needs_runtime = (start_node.type == NodeType::BinaryOp);
            if (step_known_positive) {
                if (start_needs_runtime) {
                    out << "for (int64_t i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_"
                        << node_idx << ") {\n";
                } else {
                    int64_t start_int = start_node.as.integer.val;
                    out << "for (int64_t i_val = " << start_int << "; i_val <= l_" << node_idx << "; i_val += st_"
                        << node_idx << ") {\n";
                }
                emit_for_body(true);
                out << "}\n";
            } else {
                if (start_needs_runtime) {
                    out << "for (int64_t i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_"
                        << node_idx << ") {\n";
                } else {
                    int64_t start_int = start_node.as.integer.val;
                    out << "for (int64_t i_val = " << start_int << "; i_val >= l_" << node_idx << "; i_val += st_"
                        << node_idx << ") {\n";
                }
                emit_for_body(true);
                out << "}\n";
            }
        } else if (step_known_positive) {
            out << "for (double i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_" << node_idx
                << ") {\n";
            emit_for_body(false);
            out << "}\n";
        } else if (step_known_negative) {
            out << "for (double i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_" << node_idx
                << ") {\n";
            emit_for_body(false);
            out << "}\n";
        } else {
            out << "if (st_" << node_idx << " > 0) {\n";
            out << "    for (double i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_"
                << node_idx << ") {\n";
            emit_for_body(false);
            out << "    }\n";
            out << "} else {\n";
            out << "    for (double i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_"
                << node_idx << ") {\n";
            emit_for_body(false);
            out << "    }\n";
            out << "}\n";
        }
    } else {
        out << "if (!_skip_" << node_idx << ") {\n";
        out << "if (_isint_" << node_idx << ") {\n";
        //------------------ stock counts remaining iterations: near the int64 edges a wrapped counter still satisfies the comparison
        out << "uint64_t icnt_" << node_idx << " = clx::forcount(is_" << node_idx << ", il_" << node_idx << ", ist_"
            << node_idx << ");\n";
        out << "for (int64_t i_val = is_" << node_idx << ";; i_val = static_cast<int64_t>(static_cast<uint64_t>(i_val) + "
            << "static_cast<uint64_t>(ist_" << node_idx << "))) {\n";
        emit_for_body(true);
        out << "if (icnt_" << node_idx << " == 0) break;\n";
        out << "--icnt_" << node_idx << ";\n";
        out << "}\n";
        out << "} else {\n";
        out << "if (fst_" << node_idx << " > 0) {\n";
        out << "for (double i_val = fs_" << node_idx << "; i_val <= fl_" << node_idx << "; i_val += fst_" << node_idx
            << ") {\n";
        emit_for_body(false);
        out << "}\n";
        out << "} else {\n";
        out << "for (double i_val = fs_" << node_idx << "; i_val >= fl_" << node_idx << "; i_val += fst_" << node_idx
            << ") {\n";
        emit_for_body(false);
        out << "}\n";
        out << "}\n";
        out << "}\n";
        out << "}\n";
    }

    for (size_t _hi = 0; _hi < state.hoisted_lookups.size(); ++_hi)
        out << "_fr.pop();\n";
    state.hoisted_lookups = std::move(_saved_hoisted);
    state.hoisted_cfuncs = std::move(_saved_hoisted_cf);
    restore_loop_table_hoists(_added_ht);
    out << "}\n";
}

//------------------ emitGenericForStatement: handles NodeType::GenericForStatement
void CodeEmitter::emitGenericForStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    const auto &loop = node.as.generic_for;

    //------------------ Native lowering: ipairs(vec)/pairs(vec) over a pure numeric array becomes a C++ for-loop; no triplet protocol.

    std::string_view vec_name;
    bool vec_is_int = false;
    bool lower_vec = false;
    if (loop.iter_count == 1) {
        uint32_t iter_idx = ctx.block_statements[loop.first_iter];
        if (iter_idx < ctx.nodes.size() && ctx.nodes[iter_idx].type == NodeType::CallExpression) {
            const auto &icn = ctx.nodes[iter_idx];
            uint32_t itgt = icn.as.call_expr.target;
            if (icn.as.call_expr.arg_count == 1 && itgt < ctx.nodes.size()
                && ctx.nodes[itgt].type == NodeType::Identifier && ctx.nodes[itgt].as.ident.is_global) {
                std::string_view fname(ctx.nodes[itgt].as.ident.name, ctx.nodes[itgt].as.ident.length);
                if ((fname == "ipairs" || fname == "pairs") && state.reassigned_vars.count(fname) == 0) {
                    uint32_t a0 = ctx.block_statements[icn.as.call_expr.first_arg];
                    if (a0 < ctx.nodes.size() && ctx.nodes[a0].type == NodeType::Identifier) {
                        std::string_view an(ctx.nodes[a0].as.ident.name, ctx.nodes[a0].as.ident.length);
                        if (state.pure_numeric_arrays.count(an)) {
                            lower_vec = true;
                            vec_name = an;
                            vec_is_int = state.int_numeric_arrays.count(an) > 0;
                        }
                    }
                }
            }
        }
    }

    auto emit_iter_var_decls = [&](auto &&val_expr_for) {
        for (uint32_t i = 0; i < loop.var_count; ++i) {
            uint32_t v_idx = ctx.block_statements[loop.first_var + i];
            std::string_view v_name(ctx.nodes[v_idx].as.ident.name, ctx.nodes[v_idx].as.ident.length);
            bool is_cap = ctx.nodes[v_idx].as.ident.is_captured;

            state.native_numbers.erase(std::remove(state.native_numbers.begin(), state.native_numbers.end(), v_name),
                state.native_numbers.end());
            state.native_integers.erase(std::string(v_name));

            std::string _vexpr = val_expr_for(i);
            if (is_cap) {
                out << "        clx::LUpValue l_" << v_name << ";\n";
                out << "        l_" << v_name << " = clx::make_upvalue(" << _vexpr << ");\n";
                out << "        _fr.push(*l_" << v_name << ");\n";
            } else {
                out << "        clx::LValue l_" << v_name << " = " << _vexpr << ";\n";
                out << "        ";
                emit_param_root(v_name, v_name);
            }
            locals.push_back({ v_name, is_cap });
        }
    };

    if (lower_vec) {
        out << "{\n";
        std::vector<std::string_view> _added_ht;
        emit_loop_table_hoists(loop.body_block, _added_ht);
        out << "    for (size_t _vi_" << node_idx << " = 1; _vi_" << node_idx << " <= l_" << vec_name
            << ".size(); ++_vi_" << node_idx << ") {\n";
        out << "        clx::FrameGuard _fg_gf_iter_" << node_idx << "(_fr);\n";
        if (loop.var_count >= 1)
            out << "        clx::LValue _kk_" << node_idx << " = clx::integer(static_cast<int64_t>(_vi_" << node_idx
                << "));\n";
        if (loop.var_count >= 2) {
            out << "        clx::LValue _vv_" << node_idx << " = ";
            if (vec_is_int)
                out << "clx::LValue(static_cast<int64_t>(l_" << vec_name << "[_vi_" << node_idx << " - 1]));\n";
            else
                out << "clx::LValue(static_cast<double>(l_" << vec_name << "[_vi_" << node_idx << " - 1]));\n";
        }
        size_t prev_locals = locals.size();
        emit_iter_var_decls([&](uint32_t i) -> std::string {
            if (i == 0)
                return "_kk_" + std::to_string(node_idx);
            if (i == 1)
                return "_vv_" + std::to_string(node_idx);
            return std::string("clx::LValue()");
        });
        if (loop.body_block != 0xFFFFFFFF)
            emit_node(loop.body_block);
        locals.resize(prev_locals);
        restore_loop_table_hoists(_added_ht);
        out << "    }\n}\n";
        return;
    }

    out << "{\n    clx::FrameGuard _fg_gen_for_" << node_idx << "(_fr);\n";

    out << "    clx::MultiValue _triplet_" << node_idx << ";\n";
    if (loop.iter_count > 0 && ctx.nodes[ctx.block_statements[loop.first_iter]].type == NodeType::CallExpression) {
        uint32_t iter_node = ctx.block_statements[loop.first_iter];
        const auto &call_node = ctx.nodes[iter_node];
        bool is_direct = false;
        std::string_view fname;
        uint32_t tgt = call_node.as.call_expr.target;
        if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
            fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (state.direct_callables.count(fname))
                is_direct = true;
        }

        out << "    {\n        size_t _ssave_iter = _fr.mark();\n";
        if (call_node.as.call_expr.arg_count > 0) {
            out << "        clx::LValue args_" << iter_node << "[" << call_node.as.call_expr.arg_count << "];\n";
            for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                if (a > 0)
                    out << "        _fr.push(args_" << iter_node << "[" << (a - 1) << "]);\n";
                out << "        args_" << iter_node << "[" << a << "] = ";
                emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                out << ";\n";
            }
        }

        if (is_direct) {
            if (call_node.as.call_expr.arg_count > 0) {
                out << "        _triplet_" << node_idx << " = clx::call_cfunc_direct(L, " << impl_call(fname)
                    << ", args_" << iter_node << ", " << call_node.as.call_expr.arg_count << ");\n";
            } else {
                out << "        _triplet_" << node_idx << " = " << impl_call(fname) << "(L, nullptr, 0);\n";
            }
        } else {
            std::string _cs_iter = emit_call_site(call_node.line, false, false);
            if (call_node.as.call_expr.arg_count > 0) {
                out << "        _triplet_" << node_idx << " = clx::call_function_rooted(L, ";
            } else {
                out << "        _triplet_" << node_idx << " = clx::call_function(L, ";
            }
            emit_node(tgt);
            out << ", " << (call_node.as.call_expr.arg_count > 0 ? "args_" + std::to_string(iter_node) : "nullptr")
                << ", " << call_node.as.call_expr.arg_count << ", " << _cs_iter << ");\n";
        }
        out << "        _fr.restore(_ssave_iter);\n    }\n";
    } else if (loop.iter_count > 0) {
        //------------------ the explist adjusts to three values: every item contributes one, a trailing call supplies the rest
        out << "    {\n        size_t _ssave_it = _fr.mark();\n";
        for (uint32_t k = 0; k < loop.iter_count; ++k) {
            out << "        clx::LValue _itv_" << node_idx << "_" << k << " = ";
            emit_node(ctx.block_statements[loop.first_iter + k]);
            out << ";\n        _fr.push(_itv_" << node_idx << "_" << k << ");\n";
        }
        out << "        _triplet_" << node_idx << " = clx::MultiValue({";
        for (uint32_t k = 0; k < loop.iter_count; ++k)
            out << " _itv_" << node_idx << "_" << k << ",";
        out << " }, L);\n        _fr.restore(_ssave_it);\n    }\n";
    } else {
        out << "    _triplet_" << node_idx << " = clx::MultiValue();\n";
    }

    out << "    clx::LValue _f_" << node_idx << " = (_triplet_" << node_idx << ".count > 0) ? _triplet_" << node_idx
        << "[0] : clx::LValue();\n";
    out << "    clx::LValue _s_" << node_idx << " = (_triplet_" << node_idx << ".count > 1) ? _triplet_" << node_idx
        << "[1] : clx::LValue();\n";
    out << "    clx::LValue _var_" << node_idx << " = (_triplet_" << node_idx << ".count > 2) ? _triplet_" << node_idx
        << "[2] : clx::LValue();\n";

    //------------------ Root the iterator triplet for the whole loop: _f/_s/_var live in plain locals

    out << "    _fr.push(_f_" << node_idx << ");\n";
    out << "    _fr.push(_s_" << node_idx << ");\n";
    out << "    _fr.push(_var_" << node_idx << ");\n";

    //------------------ the fourth value of the iterator explist is a to-be-closed variable, closed when the loop ends
    out << "    clx::LValue _tbc_" << node_idx << " = (_triplet_" << node_idx << ".count > 3) ? _triplet_" << node_idx
        << "[3] : clx::LValue();\n";
    out << "    _fr.push(_tbc_" << node_idx << ");\n";
    out << "    clx::CloseGuard _cg_" << node_idx << "(L, _tbc_" << node_idx << ", \"(for state)\");\n";

    std::vector<std::string_view> _added_ht;
    emit_loop_table_hoists(loop.body_block, _added_ht);

    out << "    while (true) {\n";
    out << "        clx::FrameGuard _fg_gf_iter_" << node_idx << "(_fr);\n";
    out << "        clx::LValue _args_" << node_idx << "[] = { _s_" << node_idx << ", _var_" << node_idx << " };\n";
    out << "        _fr.push(_args_" << node_idx << "[0]);\n";
    out << "        _fr.push(_args_" << node_idx << "[1]);\n";
    out << "        clx::MultiValue _res_" << node_idx << ";\n";
    out << "        if (_f_" << node_idx << ".type == clx::ValueType::Function) {\n";
    out << "            clx::LCFunction* _lcf_" << node_idx << " = static_cast<clx::LCFunction*>(_f_" << node_idx
        << ".as_pointer());\n";
    out << "            _res_" << node_idx << " = _lcf_" << node_idx << "->direct ? _lcf_" << node_idx
        << "->direct(L, _args_" << node_idx << ", 2) : _lcf_" << node_idx << "->func(L, _args_" << node_idx
        << ", 2);\n";
    out << "        } else {\n";
    std::string _cs_gf = emit_call_site(node.line, false, false);
    out << "            _res_" << node_idx << " = clx::call_function(L, _f_" << node_idx << ", _args_" << node_idx
        << ", 2, " << _cs_gf << ");\n";
    out << "        }\n";
    out << "        _fr.pop_n(2);\n";
    out << "        for (size_t m = 0; m < _res_" << node_idx << ".count; ++m)\n";
    out << "            _fr.push(_res_" << node_idx << "[m]);\n";

    out << "        _var_" << node_idx << " = (_res_" << node_idx << ".count > 0) ? _res_" << node_idx
        << "[0] : clx::LValue();\n";
    out << "        if (_var_" << node_idx << ".type == clx::ValueType::Nil) break;\n";

    size_t prev_locals = locals.size();
    emit_iter_var_decls([&](uint32_t i) -> std::string {
        return "(_res_" + std::to_string(node_idx) + ".count > " + std::to_string(i) + ") ? _res_"
            + std::to_string(node_idx) + "[" + std::to_string(i) + "] : clx::LValue()";
    });

    if (loop.body_block != 0xFFFFFFFF)
        emit_node(loop.body_block);

    locals.resize(prev_locals);
    restore_loop_table_hoists(_added_ht);
    out << "    }\n}\n";
}

}
