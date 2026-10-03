// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  statements.cpp · Statements & function defs│
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

//------------------ emitLabelStatement: handles NodeType::LabelStatement
void CodeEmitter::emitLabelStatement(const ASTNode &node, uint32_t node_idx) {
    uint32_t name_idx = node.as.label_stmt.name_ident;
    std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);

    out << "clx_lbl_" << lname << "_" << node_idx << ":;\n";
}

//------------------ emitGotoStatement: handles NodeType::GotoStatement
void CodeEmitter::emitGotoStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    uint32_t name_idx = node.as.goto_stmt.name_ident;
    std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);

    uint32_t target_label = state.goto_targets[node_idx];
    out << "goto clx_lbl_" << lname << "_" << target_label << ";\n";
}

//------------------ emitBlock: handles NodeType::Block
void CodeEmitter::emitBlock(const ASTNode &node, uint32_t node_idx, DeferredBlockScope *def) {
    bool prev_skip_braces = state.skip_block_braces;
    state.skip_block_braces = false;
    if (!prev_skip_braces)
        out << "{\n";
    size_t prev_native_count = state.native_numbers.size();
    bool needs_guard = false;
    for (uint32_t i = 0; i < node.as.block.count; ++i) {
        uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
        const auto &stmt = ctx.nodes[stmt_idx];
        if (stmt.type == NodeType::LocalDecl) {
            if (state.dead_stmts.count(stmt_idx))
                continue;
            for (uint32_t j = 0; j < stmt.as.local_decl.ident_count; ++j) {
                uint32_t id_idx = ctx.block_statements[stmt.as.local_decl.first_ident + j];
                std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                bool is_n = std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                    != state.native_numbers.end();
                bool is_cap = ctx.nodes[id_idx].as.ident.is_captured;
                if (!is_n || is_cap)
                    needs_guard = true;
            }
        }
    }
    if (needs_guard)
        out << "clx::ScopeGuard _sg_block_" << node_idx << "(L);\n";
    auto prev_hoisted = state.hoisted_locals;
    state.hoisted_locals.clear();

    bool has_goto = false;
    for (uint32_t i = 0; i < node.as.block.count; ++i) {
        uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
        auto t = ctx.nodes[stmt_idx].type;
        if (t == NodeType::GotoStatement) {
            has_goto = true;
            break;
        }
    }
    if (has_goto) {
        bool after_goto = false;
        for (uint32_t i = 0; i < node.as.block.count; ++i) {
            uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
            const auto &st = ctx.nodes[stmt_idx];
            if (st.type == NodeType::GotoStatement)
                after_goto = true;
            if (st.type == NodeType::LabelStatement)
                after_goto = false;
            if (after_goto && st.type == NodeType::LocalDecl) {
                bool hoist_last_is_call = false;
                if (st.as.local_decl.value_count > 0) {
                    uint32_t last_v
                        = ctx.block_statements[st.as.local_decl.first_value + st.as.local_decl.value_count - 1];
                    if (ctx.nodes[last_v].type == NodeType::CallExpression
                        || ctx.nodes[last_v].type == NodeType::Vararg)
                        hoist_last_is_call = true;
                }
                for (uint32_t j = 0; j < st.as.local_decl.ident_count; ++j) {
                    uint32_t id_idx = ctx.block_statements[st.as.local_decl.first_ident + j];
                    std::string_view nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                    if (state.hoisted_locals.count(nm))
                        continue;
                    bool is_cap = ctx.nodes[id_idx].as.ident.is_captured;
                    bool in_native = std::find(state.native_numbers.begin(), state.native_numbers.end(), nm)
                        != state.native_numbers.end();
                    if (is_cap) {
                        out << "clx::LUpValue l_" << nm << ";\n";
                        out << "l_" << nm << " = clx::make_upvalue(clx::LValue());\n";
                        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << nm << "->val, &l_" << nm
                            << "->type);\n";
                    } else if (in_native) {
                        out << "double l_" << nm << ";\n";
                        out << "bool _intf_l_" << nm << " = false;\n";
                        out << "int64_t _ii_l_" << nm << " = 0;\n";
                    } else {
                        out << "clx::LValue l_" << nm << ";\n";
                        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << nm << ".val, &l_" << nm
                            << ".type);\n";
                    }
                    state.hoisted_locals.insert(nm);
                }
                if (hoist_last_is_call) {
                    out << "clx::MultiValue _mret_" << stmt_idx << ";\n";
                }
            }
        }
    }

    for (uint32_t _pi = 0; _pi < node.as.block.count; ++_pi) {
        uint32_t _ps = ctx.block_statements[node.as.block.first_statement + _pi];
        const auto &_pst = ctx.nodes[_ps];
        if (_pst.type != NodeType::LocalDecl && _pst.type != NodeType::GlobalDeclStatement
            && _pst.type != NodeType::Assignment)
            continue;
        uint32_t _ptc = _pst.type == NodeType::LocalDecl
            ? _pst.as.local_decl.ident_count
            : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.ident_count
                                                          : _pst.as.assign.target_count);
        uint32_t _pvc = _pst.type == NodeType::LocalDecl
            ? _pst.as.local_decl.value_count
            : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.value_count
                                                          : _pst.as.assign.value_count);
        uint32_t _pfv = _pst.type == NodeType::LocalDecl
            ? _pst.as.local_decl.first_value
            : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.first_value
                                                          : _pst.as.assign.first_value);
        uint32_t _pft = _pst.type == NodeType::LocalDecl
            ? _pst.as.local_decl.first_ident
            : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.first_ident
                                                          : _pst.as.assign.first_target);
        for (uint32_t _pj = 0; _pj < _pvc && _pj < _ptc; ++_pj) {
            uint32_t _pvi = ctx.block_statements[_pfv + _pj];
            if (ctx.nodes[_pvi].type != NodeType::FunctionDef)
                continue;
            uint32_t _pti = ctx.block_statements[_pft + _pj];
            if (ctx.nodes[_pti].type != NodeType::Identifier || ctx.nodes[_pti].as.ident.is_global)
                continue;
            std::string_view _pn(ctx.nodes[_pti].as.ident.name, ctx.nodes[_pti].as.ident.length);
            if (state.reassigned_vars.count(_pn))
                continue;
            bool _pf = false;
            if (state.native_return_funcs.count(_pn) && state.func_param_native.count(_pn)) {
                _pf = true;
                for (bool _pp : state.func_param_native[_pn])
                    if (!_pp) {
                        _pf = false;
                        break;
                    }
            }
            if (_pf)
                state.fast_callables.insert(_pn);
            state.direct_callables.insert(_pn);
        }
    }

    size_t prev_locals = locals.size();
    int redecl_scopes = 0;
    std::set<std::string_view> current_block_vars;

    for (uint32_t i = 0; i < node.as.block.count; ++i) {
        uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
        if (state.dead_stmts.count(stmt_idx))
            continue;
        const auto &stmt = ctx.nodes[stmt_idx];

        if (stmt.type == NodeType::LocalDecl) {
            bool creates_shadow = false;
            for (uint32_t j = 0; j < stmt.as.local_decl.ident_count; ++j) {
                uint32_t id_idx = ctx.block_statements[stmt.as.local_decl.first_ident + j];
                std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                if (current_block_vars.count(name))
                    creates_shadow = true;
            }
            if (creates_shadow) {
                out << "{\n";
                redecl_scopes++;
                current_block_vars.clear();
            }
            for (uint32_t j = 0; j < stmt.as.local_decl.ident_count; ++j) {
                uint32_t id_idx = ctx.block_statements[stmt.as.local_decl.first_ident + j];
                std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                current_block_vars.insert(name);
            }
        }

        if (stmt.type == NodeType::CallExpression) {
            out << "#line " << stmt.line << " \"" << ctx.filename << "\"\n";
        }
        emit_node(stmt_idx);
        if (stmt.type != NodeType::Block && stmt.type != NodeType::Assignment
            && stmt.type != NodeType::GlobalDeclStatement && stmt.type != NodeType::LocalDecl
            && stmt.type != NodeType::BreakStatement && stmt.type != NodeType::ReturnStatement
            && stmt.type != NodeType::IfStatement && stmt.type != NodeType::WhileStatement
            && stmt.type != NodeType::RepeatStatement && stmt.type != NodeType::ForStatement
            && stmt.type != NodeType::GenericForStatement && stmt.type != NodeType::GotoStatement
            && stmt.type != NodeType::LabelStatement && stmt.type != NodeType::DoStatement) {
            out << ";\n";
        }
    }

    if (def) {
        def->prev_locals = prev_locals;
        def->prev_native_count = prev_native_count;
        def->prev_hoisted = prev_hoisted;
        def->close_braces = redecl_scopes;
        def->emit_close = !prev_skip_braces;
        return;
    }

    for (int i = 0; i < redecl_scopes; ++i) {
        out << "}\n";
    }

    locals.resize(prev_locals);
    state.native_numbers.resize(prev_native_count);
    state.hoisted_locals = prev_hoisted;
    state.skip_block_braces = prev_skip_braces;
    if (!prev_skip_braces)
        out << "}\n";
}

//------------------ emit_native_impl_def: Y-combinator native overload for a

void CodeEmitter::emit_native_impl_def(std::string_view name, uint32_t v_idx) {
    const auto &fn = ctx.nodes[v_idx];
    uint32_t nparams = fn.as.func_def.param_count;
    out << "auto native_" << name << "_impl = [=](auto& self, clx::LState* L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", clx::LValue p" << a;
    out << ") -> clx::LValue {\n";

    state.native_emitted.insert(name);
    auto saved_native_func = state.current_native_func;
    bool saved_in_native = state.in_native_impl;
    state.emit_native_impl = true;
    state.in_native_impl = true;
    state.current_native_func = name;
    emit_node(v_idx);
    state.emit_native_impl = false;
    state.in_native_impl = saved_in_native;
    state.current_native_func = saved_native_func;
    out << ";\n";
    out << "#line " << fn.line << " \"" << ctx.filename << "\"\n";
    out << "auto native_" << name << " = [=](clx::LState* L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", clx::LValue p" << a;
    out << ") -> clx::LValue { return native_" << name << "_impl(native_" << name << "_impl, L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", p" << a;
    out << "); };\n";
}

//------------------ emitFunctionDef: handles NodeType::FunctionDef
void CodeEmitter::emitFunctionDef(const ASTNode &node, uint32_t node_idx) {
    bool is_raw = state.emit_raw_lambda;
    bool is_fast = state.emit_fast_lambda;
    bool is_native = state.emit_native_impl;
    state.emit_raw_lambda = false;
    state.emit_fast_lambda = false;
    state.emit_native_impl = false;
    state.emit_native_impl = false;
    bool prev_in_func = state.in_function_def;
    state.in_function_def = true;

    auto saved_direct_callables = state.direct_callables;
    auto saved_fast_callables = state.fast_callables;
    auto saved_native_emitted = state.native_emitted;
    auto saved_hoisted = state.hoisted_tables;
    bool saved_in_native = state.in_native_impl;
    std::string_view saved_native_func = state.current_native_func;
    if (!is_native) {

        state.in_native_impl = false;
        state.current_native_func = "";
    }
    uint32_t saved_func_body = state.current_func_body;
    state.current_func_body = node.as.func_def.body_block;

    if (is_fast) {
        std::unordered_map<std::string_view, int> fast_param_counts;
        std::vector<std::string> fast_param_cpp;
        fast_param_cpp.reserve(node.as.func_def.param_count);
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
            std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
            int cnt = fast_param_counts[pname]++;
            std::string unique = (cnt == 0) ? std::string(pname) : std::string(pname) + std::to_string(cnt);
            fast_param_cpp.push_back(std::move(unique));
        }
        out << "[=](auto& self";
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            out << ", double l_" << fast_param_cpp[i];
        }
        out << ") -> double {\n";

        size_t prev_locals = locals.size();
        size_t prev_native_count = state.native_numbers.size();
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
            std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
            locals.push_back({ pname, fast_param_cpp[i], false });
            if (std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                == state.native_numbers.end())
                state.native_numbers.push_back(pname);
        }

        if (node.as.func_def.body_block != 0xFFFFFFFF)
            emit_node(node.as.func_def.body_block);

        locals.resize(prev_locals);
        state.native_numbers.resize(prev_native_count);
        state.in_function_def = false;
        state.direct_callables = std::move(saved_direct_callables);
        state.fast_callables = std::move(saved_fast_callables);
        state.native_emitted = std::move(saved_native_emitted);
        state.hoisted_tables = std::move(saved_hoisted);
        state.in_native_impl = saved_in_native;
        state.current_native_func = saved_native_func;
        state.current_func_body = saved_func_body;
        out << "return 0.0;\n}";
        return;
    }

    if (is_native) {

        out << "size_t _sg_native_" << node_idx << " = L->shadow_top;\n";
        uint32_t prev_func_idx_n = state.current_func_idx;
        state.current_func_idx = node_idx;
        uint32_t saved_arena_native = state.current_arena_func;
        if (state.arena_table_sizes.count(node_idx)) {
            state.current_arena_func = node_idx;
            out << "clx::FuncArena _arena;\n";
            out << "clx::arena_init(&_arena, " << state.arena_table_sizes[node_idx] << ");\n";
        } else {
            state.current_arena_func = 0xFFFFFFFF;
        }

        size_t prev_native_count_n = state.native_numbers.size();
        size_t prev_locals_n = locals.size();
        std::unordered_map<std::string_view, int> nparam_name_counts;
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
            std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
            int cnt = nparam_name_counts[pname]++;
            std::string cpp_name = (cnt == 0) ? std::string(pname) : std::string(pname) + std::to_string(cnt);
            bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
            bool is_native_p = std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                    != state.native_numbers.end()
                || (!is_cap && state.param_numbers[node_idx].count(std::string(pname)));
            if (is_cap) {
                if (state.constant_upvalues.count(pname)) {
                    out << "clx::LValue l_" << cpp_name << " = p" << i << ";\n";
                    out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << cpp_name << ".val, &l_"
                        << cpp_name << ".type);\n";
                    if (!state.pure_numeric_arrays.count(pname)) {
                        out << "auto l_" << cpp_name << "_csnap = clx::make_upvalue(l_" << cpp_name << ");\n";
                        state.const_snapshot_cells.push_back(std::string(cpp_name));
                    }
                } else {
                    out << "clx::LUpValue l_" << cpp_name << " = clx::make_upvalue(p" << i << ");\n";
                    out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << cpp_name << "->val, &l_"
                        << cpp_name << "->type);\n";
                }
            } else if (is_native_p) {
                out << "double l_" << cpp_name << " = p" << i << ".as_number();\n";
                out << "bool _intf_l_" << cpp_name << " = p" << i << ".type == clx::ValueType::Int64;\n";
                out << "int64_t _ii_l_" << cpp_name << " = p" << i << ".as_integer();\n";
                if (std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                    == state.native_numbers.end())
                    state.native_numbers.push_back(pname);
            } else {
                out << "clx::LValue l_" << cpp_name << " = p" << i << ";\n";
                emit_param_root(pname, cpp_name);
            }
            if (state.string_builders.count(pname) && !state.global_string_builders.count(pname)
                && !state.module_string_builders.count(pname)) {
                out << "clx::StringBuilder sb_" << cpp_name << ";\n";
            }
            bool nparam_is_boxed = is_cap && !state.constant_upvalues.count(pname);
            locals.push_back({ pname, cpp_name, nparam_is_boxed });
            if (state.string_builders.count(pname) && !state.global_string_builders.count(pname)
                && !state.module_string_builders.count(pname))
                locals.back().has_sb = true;
            if (!nparam_is_boxed) {
                bool np_native = std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                    != state.native_numbers.end();
                locals.back().has_intf = np_native;
                locals.back().has_ii = np_native;
            }
        }

        if (node.as.func_def.body_block != 0xFFFFFFFF)
            emit_node(node.as.func_def.body_block);

        locals.resize(prev_locals_n);
        state.native_numbers.resize(prev_native_count_n);
        state.current_func_idx = prev_func_idx_n;
        state.current_arena_func = saved_arena_native;
        state.in_function_def = prev_in_func;
        state.in_native_impl = saved_in_native;
        state.current_native_func = saved_native_func;
        state.native_emitted = std::move(saved_native_emitted);
        state.hoisted_tables = std::move(saved_hoisted);
        state.current_func_body = saved_func_body;
        if (state.arena_table_sizes.count(node_idx))
            out << "clx::arena_reset(&_arena);\n";
        out << "L->shadow_top = _sg_native_" << node_idx << ";\n";
        out << "return clx::LValue();\n}";
        return;
    }

    if (!is_raw)
        out << "L->create_closure(";
    if (!state.ref_capture.empty()) {
        out << "[=, &" << state.ref_capture << "]";
    } else {
        out << "[=]";
    }
    out << "(clx::LState* L, const clx::LValue* args, size_t arg_count) mutable -> clx::MultiValue {\n";
    if (!state.raw_lambda_cell.empty()) {
        bool param_shadows = false;
        for (uint32_t pi = 0; pi < node.as.func_def.param_count; ++pi) {
            uint32_t pp = ctx.block_statements[node.as.func_def.first_param + pi];
            if (pp < ctx.nodes.size() && ctx.nodes[pp].type == NodeType::Identifier
                && std::string_view(ctx.nodes[pp].as.ident.name, ctx.nodes[pp].as.ident.length)
                    == state.raw_lambda_cell) {
                param_shadows = true;
                break;
            }
        }
        if (!param_shadows)
            out << "const clx::LValue& l_" << state.raw_lambda_cell << " = *l_" << state.raw_lambda_cell << "_cell;\n";
    }
    out << "clx::LValue _ENV = (L->current_func && L->current_func->env) ? clx::LValue(clx::ValueType::Table, "
           "L->current_func->env) : clx::LValue(clx::ValueType::Table, L->_G);\n";
    uint32_t saved_arena_func = state.current_arena_func;
    if (state.arena_table_sizes.count(node_idx)) {
        state.current_arena_func = node_idx;
        out << "clx::FuncArena _arena;\n";
        out << "clx::arena_init(&_arena, " << state.arena_table_sizes[node_idx] << ");\n";
    } else {
        state.current_arena_func = 0xFFFFFFFF;
    }
    out << "size_t _va_count = (arg_count > " << node.as.func_def.param_count << ") ? (arg_count - "
        << node.as.func_def.param_count << ") : 0;\n";
    out << "const clx::LValue* _va_args = _va_count > 0 ? (args + " << node.as.func_def.param_count << ") : nullptr;\n";

    uint32_t prev_func_idx = state.current_func_idx;
    state.current_func_idx = node_idx;
    out << "size_t _sg_func_" << node_idx << " = L->shadow_top;\n";

    size_t prev_native_count_for_params = state.native_numbers.size();

    std::unordered_map<std::string_view, int> param_name_counts;
    std::vector<std::string> param_cpp_names;
    param_cpp_names.reserve(node.as.func_def.param_count);
    for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
        uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
        int cnt = param_name_counts[pname]++;
        std::string unique;
        if (cnt == 0)
            unique = std::string(pname);
        else
            unique = std::string(pname) + std::to_string(cnt);
        param_cpp_names.push_back(std::move(unique));
    }
    std::string vararg_cpp_name;
    if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
        std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name,
            ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
        auto it = param_name_counts.find(vaname);
        if (it != param_name_counts.end()) {
            vararg_cpp_name = std::string(vaname) + std::to_string(it->second);
            param_name_counts[vaname]++;
        } else {
            vararg_cpp_name = std::string(vaname);
            param_name_counts[vaname] = 1;
        }
    }

    for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
        uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
        const std::string &cpp_name = param_cpp_names[i];
        bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
        bool is_native
            = std::find(state.native_numbers.begin(), state.native_numbers.end(), pname) != state.native_numbers.end()
            || (!is_cap && state.param_numbers[node_idx].count(std::string(pname)));

        if (is_cap) {
            if (state.constant_upvalues.count(pname)) {
                out << "clx::LValue l_" << cpp_name << " = (" << i << " < arg_count) ? args[" << i
                    << "] : clx::LValue();\n";
                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << cpp_name << ".val, &l_" << cpp_name
                    << ".type);\n";
                if (!state.pure_numeric_arrays.count(pname)) {
                    out << "auto l_" << cpp_name << "_csnap = clx::make_upvalue(l_" << cpp_name << ");\n";
                    state.const_snapshot_cells.push_back(std::string(cpp_name));
                }
            } else {
                out << "clx::LUpValue l_" << cpp_name << ";\n";
                out << "l_" << cpp_name << " = clx::make_upvalue((" << i << " < arg_count) ? args[" << i
                    << "] : clx::LValue());\n";
                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << cpp_name << "->val, &l_" << cpp_name
                    << "->type);\n";
            }
        } else if (is_native) {
            out << "double l_" << cpp_name << " = (" << i << " < arg_count) ? args[" << i << "].as_number() : 0.0;\n";
            out << "bool _intf_l_" << cpp_name << " = (" << i << " < arg_count) && args[" << i
                << "].type == clx::ValueType::Int64;\n";
            out << "int64_t _ii_l_" << cpp_name << " = (" << i << " < arg_count) ? args[" << i
                << "].as_integer() : 0;\n";
            if (std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                == state.native_numbers.end())
                state.native_numbers.push_back(pname);
        } else {
            out << "clx::LValue l_" << cpp_name << " = (" << i << " < arg_count) ? args[" << i
                << "] : clx::LValue();\n";
            emit_param_root(pname, cpp_name);
        }
        if (state.string_builders.count(pname) && !state.global_string_builders.count(pname)
            && !state.module_string_builders.count(pname)) {
            out << "clx::StringBuilder sb_" << cpp_name << ";\n";
        }
    }

    if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
        std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name,
            ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
        out << "clx::LValue l_" << vararg_cpp_name << " = L->create_table(_va_count, 1);\n";
        out << "clx::LTable* _t_" << vararg_cpp_name << " = static_cast<clx::LTable*>(l_" << vararg_cpp_name
            << ".as_pointer());\n";
        out << "_t_" << vararg_cpp_name
            << "->settable(clx::LValue(L->intern_string(\"n\")), clx::LValue(static_cast<double>(_va_count)));\n";
        out << "for (size_t i = 0; i < _va_count; ++i) {\n";
        out << "    _t_" << vararg_cpp_name << "->settable(clx::LValue(static_cast<double>(i + 1)), _va_args[i]);\n";
        out << "}\n";
        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << vararg_cpp_name << ".val, &l_"
            << vararg_cpp_name << ".type);\n";
    }

    size_t prev_locals = locals.size();
    for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
        uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
        const std::string &cpp_name = param_cpp_names[i];
        bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
        bool param_is_boxed = is_cap && !state.constant_upvalues.count(pname);
        locals.push_back({ pname, cpp_name, param_is_boxed });
        if (state.string_builders.count(pname) && !state.global_string_builders.count(pname)
            && !state.module_string_builders.count(pname))
            locals.back().has_sb = true;
        if (!param_is_boxed) {
            bool p_native = std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                != state.native_numbers.end();
            locals.back().has_intf = p_native;
            locals.back().has_ii = p_native;
        }
    }
    if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
        std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name,
            ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
        locals.push_back({ vaname, vararg_cpp_name, false });
    }

    if (node.as.func_def.body_block != 0xFFFFFFFF) {
        emit_node(node.as.func_def.body_block);
    }

    locals.resize(prev_locals);
    state.native_numbers.resize(prev_native_count_for_params);
    state.in_function_def = prev_in_func;
    state.in_native_impl = saved_in_native;
    state.current_native_func = saved_native_func;
    state.native_emitted = std::move(saved_native_emitted);
    state.hoisted_tables = std::move(saved_hoisted);
    state.current_func_body = saved_func_body;
    state.current_func_idx = prev_func_idx;
    state.direct_callables = std::move(saved_direct_callables);
    state.fast_callables = std::move(saved_fast_callables);
    if (state.arena_table_sizes.count(node_idx))
        out << "clx::arena_reset(&_arena);\n";
    state.current_arena_func = saved_arena_func;
    out << "L->shadow_top = _sg_func_" << node_idx << ";\n";
    out << "return clx::MultiValue();\n";
    out << "}";
    if (!is_raw) {
        out << ", static_cast<clx::LTable*>(_ENV.as_pointer())";
        std::string cells = gc_cells_arg(std::string(state.raw_lambda_cell));
        if (!cells.empty())
            out << ", " << cells;
        out << ")";
    }
}

//------------------ emitReturnStatement: handles NodeType::ReturnStatement
void CodeEmitter::emitReturnStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    uint32_t v_count = node.as.return_stmt.value_count;
    uint32_t first_v = node.as.return_stmt.first_value;

    auto emit_shadow_restore = [&]() {
        if (state.in_function_def && state.current_func_idx != 0xFFFFFFFF)
            out << "L->shadow_top = _sg_func_" << state.current_func_idx << ";\n";
    };

    if (state.in_fast_function) {
        if (v_count == 0) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
            out << "return 0.0;\n";
        } else {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
            out << "return ";
            emit_native(ctx.block_statements[first_v]);
            out << ";\n";
        }
        return;
    }

    if (state.in_native_impl) {

        if (state.current_arena_func != 0xFFFFFFFF)
            out << "clx::arena_reset(&_arena);\n";
        if (state.current_func_idx != 0xFFFFFFFF)
            out << "L->shadow_top = _sg_native_" << state.current_func_idx << ";\n";
        if (v_count == 0) {
            out << "return clx::LValue();\n";
        } else {
            out << "return ";
            emit_node(ctx.block_statements[first_v]);
            out << ";\n";
        }
        return;
    }

    if (v_count == 0) {
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
            emit_shadow_restore();
            out << "return clx::MultiValue();\n";
        } else {
            out << "return clx::LValue();\n";
        }
        return;
    }

    bool last_is_call = false;
    uint32_t last_v_idx = ctx.block_statements[first_v + v_count - 1];
    if (ctx.nodes[last_v_idx].type == NodeType::CallExpression) {
        last_is_call = true;
    }

    if (v_count == 1 && last_is_call) {
        const auto &call_node = ctx.nodes[last_v_idx];
        bool is_direct = false;
        std::string_view fname;
        uint32_t tgt = call_node.as.call_expr.target;
        if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
            fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (state.direct_callables.count(fname))
                is_direct = true;
        }

        //------------------ native-direct tailcall: exact-1 callee, exact

        if (!fname.empty() && state.in_function_def
            && native_call_eligible(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count)) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
            if (state.in_native_impl) {
                if (state.current_func_idx != 0xFFFFFFFF)
                    out << "L->shadow_top = _sg_native_" << state.current_func_idx << ";\n";
                out << "return ";
                try_emit_native_call(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count, false);
                out << ";\n";
            } else {
                emit_shadow_restore();
                out << "return clx::MultiValue(";
                try_emit_native_call(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count, false);
                out << ");\n";
            }
            return;
        }

        out << "{\n";
        bool last_expands = false;
        uint32_t last_arg = 0xFFFFFFFF;
        if (call_node.as.call_expr.arg_count > 0) {
            last_arg = ctx.block_statements[call_node.as.call_expr.first_arg + call_node.as.call_expr.arg_count - 1];
            if (ctx.nodes[last_arg].type == NodeType::CallExpression || ctx.nodes[last_arg].type == NodeType::Vararg)
                last_expands = true;
        }

        if (last_expands) {
            out << "    size_t _ssave_dyn = L->shadow_top;\n";
            out << "    clx::LValue _dyn_buf[16];\n    size_t _dyn_count = 0;\n";
            for (uint32_t a = 0; a < call_node.as.call_expr.arg_count - 1; ++a) {
                if (a > 0)
                    out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_dyn_buf[" << (a - 1)
                        << "].val, &_dyn_buf[" << (a - 1) << "].type);\n";
                out << "    _dyn_buf[_dyn_count++] = ";
                emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                out << ";\n";
            }
            state.expect_multivalue = true;
            out << "    clx::MultiValue _mret = ";
            emit_node(last_arg);
            out << ";\n";
            state.expect_multivalue = false;
            out << "    for (size_t _mi = 0; _mi < _mret.count; ++_mi) _dyn_buf[_dyn_count++] = _mret[_mi];\n";
            if (is_direct) {
                if (state.in_function_def) {
                    if (state.current_arena_func != 0xFFFFFFFF)
                        out << "    clx::arena_reset(&_arena);\n";
                    out << "    for (size_t i = 0; i < _dyn_count; ++i) L->shadow_stack[L->shadow_top++] = "
                           "clx::TypedSlot(&_dyn_buf[i].val, &_dyn_buf[i].type);\n";
                    out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname)
                        << ", _dyn_buf, _dyn_count);\n";
                    emit_shadow_restore();
                    out << "    return _res;\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname)
                        << ", _dyn_buf, _dyn_count);\n";
                    out << "    L->shadow_top = _ssave_dyn;\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else if (!fname.empty() && state.builtin_aliases.count(std::string(fname))
                && state.reassigned_vars.count(std::string(fname)) == 0) {
                const std::string &_bi_cf = state.builtin_aliases.at(std::string(fname));
                if (state.in_function_def) {
                    out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf
                        << ", _dyn_buf, _dyn_count);\n";
                    emit_shadow_restore();
                    out << "    return _res;\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf
                        << ", _dyn_buf, _dyn_count);\n";
                    out << "    L->shadow_top = _ssave_dyn;\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else if (state.in_function_def) {
                if (state.current_arena_func != 0xFFFFFFFF)
                    out << "    clx::arena_reset(&_arena);\n";
                out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                emit_node(tgt);
                out << ", _dyn_buf, _dyn_count, \"" << ctx.filename << "\", " << call_node.line << ");\n";
                emit_shadow_restore();
                out << "    return _res;\n";
            } else {
                out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                emit_node(tgt);
                out << ", _dyn_buf, _dyn_count, \"" << ctx.filename << "\", " << call_node.line << ");\n";
                out << "    L->shadow_top = _ssave_dyn;\n";
                out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
            }
        } else {
            if (call_node.as.call_expr.arg_count > 0) {
                out << "    size_t _ssave_ret = L->shadow_top;\n";
                out << "    clx::LValue args_" << last_v_idx << "[" << call_node.as.call_expr.arg_count << "];\n";
                for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                    if (a > 0)
                        out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args_" << last_v_idx << "["
                            << (a - 1) << "].val, &args_" << last_v_idx << "[" << (a - 1) << "].type);\n";
                    out << "    args_" << last_v_idx << "[" << a << "] = ";
                    emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                    out << ";\n";
                }
                if (is_direct) {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname) << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        emit_shadow_restore();
                        out << "    return _res;\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname) << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        out << "    L->shadow_top = _ssave_ret;\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else if (!fname.empty() && state.builtin_aliases.count(std::string(fname))
                    && state.reassigned_vars.count(std::string(fname)) == 0) {
                    const std::string &_bi_cf = state.builtin_aliases.at(std::string(fname));
                    if (state.in_function_def) {
                        out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        emit_shadow_restore();
                        out << "    return _res;\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        out << "    L->shadow_top = _ssave_ret;\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else if (state.in_function_def) {
                    if (state.current_arena_func != 0xFFFFFFFF)
                        out << "    clx::arena_reset(&_arena);\n";
                    out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                    emit_node(tgt);
                    out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", \"" << ctx.filename
                        << "\", " << call_node.line << ");\n";
                    emit_shadow_restore();
                    out << "    return _res;\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                    emit_node(tgt);
                    out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", \"" << ctx.filename
                        << "\", " << call_node.line << ");\n";
                    out << "    L->shadow_top = _ssave_ret;\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else {
                if (is_direct) {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        emit_shadow_restore();
                        out << "    CLX_MUSTTAIL return " << impl_call(fname) << "(L, nullptr, 0);\n";
                    } else {
                        out << "    clx::MultiValue _res = " << impl_call(fname) << "(L, nullptr, 0);\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else if (!fname.empty() && state.builtin_aliases.count(std::string(fname))
                    && state.reassigned_vars.count(std::string(fname)) == 0) {
                    const std::string &_bi_cf = state.builtin_aliases.at(std::string(fname));
                    if (state.in_function_def) {
                        emit_shadow_restore();
                        out << "    CLX_MUSTTAIL return clx::" << _bi_cf << "(L, nullptr, 0);\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::" << _bi_cf << "(L, nullptr, 0);\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    CLX_MUSTTAIL return clx::call_function(L, ";
                        emit_node(tgt);
                        out << ", nullptr, 0, \"" << ctx.filename << "\", " << call_node.line << ");\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_function(L, ";
                        emit_node(tgt);
                        out << ", nullptr, 0, \"" << ctx.filename << "\", " << call_node.line << ");\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                }
            }
        }
        out << "}\n";
        return;
    }

    bool has_vararg = false;
    for (size_t i = 0; i < v_count; ++i) {
        if (ctx.nodes[ctx.block_statements[first_v + i]].type == NodeType::Vararg)
            has_vararg = true;
    }

    out << "{\n";
    if (!last_is_call && !has_vararg) {
        out << "    size_t _eg_rbase = L->shadow_top;\n";
        out << "    clx::LValue _ret_args[" << std::max(1u, v_count) << "];\n";
        for (size_t i = 0; i < v_count; ++i) {
            out << "    _ret_args[" << i << "] = ";
            emit_node(ctx.block_statements[first_v + i]);
            out << ";\n";
            out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_ret_args[" << i << "].val, &_ret_args[" << i
                << "].type);\n";
        }
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "    clx::arena_reset(&_arena);\n";
            emit_shadow_restore();
            out << "    return clx::MultiValue(_ret_args, " << v_count << ", L);\n";
        } else {
            out << "    L->shadow_top = _eg_rbase;\n";
            out << "    return _ret_args[0];\n";
        }
    } else {
        out << "    size_t _eg_rbase = L->shadow_top;\n";
        out << "    std::vector<clx::LValue> _ret_vals;\n";
        for (size_t i = 0; i < v_count; ++i) {
            uint32_t v_idx = ctx.block_statements[first_v + i];
            if (i == v_count - 1 && (last_is_call || ctx.nodes[v_idx].type == NodeType::Vararg)) {
                state.expect_multivalue = true;
                out << "    clx::MultiValue _mret = ";
                emit_node(v_idx);
                out << ";\n";
                state.expect_multivalue = false;
                out << "    for (size_t m = 0; m < _mret.count; ++m) {\n";
                out << "        _ret_vals.push_back(_mret[m]);\n";
                out << "        L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_mret[m].val, &_mret[m].type);\n";
                out << "    }\n";
            } else {
                out << "    clx::LValue _tmp_" << node_idx << "_" << i << " = ";
                emit_node(v_idx);
                out << ";\n";
                out << "    _ret_vals.push_back(_tmp_" << node_idx << "_" << i << ");\n";
                out << "    L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_tmp_" << node_idx << "_" << i
                    << ".val, &_tmp_" << node_idx << "_" << i << ".type);\n";
            }
        }
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "    clx::arena_reset(&_arena);\n";
            emit_shadow_restore();
            out << "    return clx::MultiValue(_ret_vals, L);\n";
        } else {
            out << "    L->shadow_top = _eg_rbase;\n";
            out << "    return (!_ret_vals.empty()) ? _ret_vals[0] : clx::LValue();\n";
        }
    }
    out << "}\n";
}

//------------------ emitDoStatement: handles NodeType::DoStatement
void CodeEmitter::emitDoStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    size_t prev_locals = locals.size();
    if (node.as.do_stmt.body_block != 0xFFFFFFFF)
        emit_node(node.as.do_stmt.body_block);
    locals.resize(prev_locals);
}

//------------------ emitIfStatement: handles NodeType::IfStatement
void CodeEmitter::emitIfStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    out << "if (";
    emit_condition(node.as.if_stmt.condition);
    out << ")\n";
    if (node.as.if_stmt.then_block != 0xFFFFFFFF)
        emit_node(node.as.if_stmt.then_block);
    if (node.as.if_stmt.else_block != 0xFFFFFFFF) {
        out << "else\n";
        emit_node(node.as.if_stmt.else_block);
    }
}

//------------------ emitWhileStatement: handles NodeType::WhileStatement
void CodeEmitter::emitWhileStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    out << "{\n";
    std::vector<std::string_view> _added_ht;
    emit_loop_table_hoists(node.as.while_stmt.body_block, _added_ht);
    out << "while (";
    emit_condition(node.as.while_stmt.condition);
    out << ")\n";
    if (node.as.while_stmt.body_block != 0xFFFFFFFF)
        emit_node(node.as.while_stmt.body_block);
    restore_loop_table_hoists(_added_ht);
    out << "}\n";
}

//------------------ emitRepeatStatement: handles NodeType::RepeatStatement
void CodeEmitter::emitRepeatStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    out << "{\n";
    std::vector<std::string_view> _added_ht;
    emit_loop_table_hoists(node.as.repeat_stmt.body_block, _added_ht);
    out << "do\n";
    DeferredBlockScope defer;
    bool saved_skip = state.skip_block_braces;
    if (node.as.repeat_stmt.body_block != 0xFFFFFFFF) {
        const ASTNode &body = ctx.nodes[node.as.repeat_stmt.body_block];
        bool body_is_block = body.type == NodeType::Block;
        state.skip_block_braces = false;
        if (body_is_block)
            emitBlock(body, node.as.repeat_stmt.body_block, &defer);
        else
            emit_node(node.as.repeat_stmt.body_block);
    }
    out << "if (";
    emit_condition(node.as.repeat_stmt.condition);
    out << ") break;\n";
    for (int i = 0; i < defer.close_braces; ++i)
        out << "}\n";
    if (defer.emit_close)
        out << "}\n";
    locals.resize(defer.prev_locals);
    state.native_numbers.resize(defer.prev_native_count);
    state.hoisted_locals = defer.prev_hoisted;
    state.skip_block_braces = saved_skip;
    restore_loop_table_hoists(_added_ht);
    out << "while (true);\n";
    out << "}\n";
}

//------------------ emitBreakStatement: handles NodeType::BreakStatement
void CodeEmitter::emitBreakStatement(const ASTNode &node, uint32_t node_idx) {
    out << "break;";
}

}
