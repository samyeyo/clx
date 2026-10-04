// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  codegen.cpp · C++ Code Generator           │
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

//------------------ var_reassigned_non_int: checks if a variable receives any non-integer value
bool CodeEmitter::var_reassigned_non_int(std::string_view name, uint32_t block_idx, uint32_t exclude_value_node) {
    auto walk_block = [&](auto &self, uint32_t bi) -> bool {
        if (bi == 0xFFFFFFFF || bi >= ctx.nodes.size())
            return false;
        const auto &block = ctx.nodes[bi];
        if (block.type != NodeType::Block)
            return false;
        for (uint32_t si = 0; si < block.as.block.count; ++si) {
            uint32_t stmt = ctx.block_statements[block.as.block.first_statement + si];
            if (stmt >= ctx.nodes.size())
                continue;
            const auto &sn = ctx.nodes[stmt];
            auto check_assign = [&](uint32_t target_idx, uint32_t value_idx) -> bool {
                if (target_idx >= ctx.nodes.size() || ctx.nodes[target_idx].type != NodeType::Identifier)
                    return false;
                std::string_view tn(ctx.nodes[target_idx].as.ident.name, ctx.nodes[target_idx].as.ident.length);
                if (tn != name)
                    return false;
                if (value_idx >= ctx.nodes.size())
                    return false;
                if (value_idx == exclude_value_node)
                    return false;
                return !clx::is_purely_integer_expr(ctx, state, value_idx);
            };
            if (sn.type == NodeType::Assignment) {
                for (uint32_t ti = 0; ti < sn.as.assign.target_count; ++ti) {
                    uint32_t tgt = ctx.block_statements[sn.as.assign.first_target + ti];
                    uint32_t val = (ti < sn.as.assign.value_count) ? ctx.block_statements[sn.as.assign.first_value + ti]
                                                                   : 0xFFFFFFFF;
                    if (check_assign(tgt, val))
                        return true;
                }
            }
            if (sn.type == NodeType::LocalDecl) {
                for (uint32_t ti = 0; ti < sn.as.local_decl.ident_count; ++ti) {
                    uint32_t tgt = ctx.block_statements[sn.as.local_decl.first_ident + ti];
                    uint32_t val = (ti < sn.as.local_decl.value_count)
                        ? ctx.block_statements[sn.as.local_decl.first_value + ti]
                        : 0xFFFFFFFF;
                    if (check_assign(tgt, val))
                        return true;
                }
            }
            auto recurse_block = [&](uint32_t bi2) { return self(self, bi2); };
            if (sn.type == NodeType::Block) {
                if (recurse_block(stmt))
                    return true;
            }
            if (sn.type == NodeType::WhileStatement) {
                if (recurse_block(sn.as.while_stmt.body_block))
                    return true;
            }
            if (sn.type == NodeType::RepeatStatement) {
                if (recurse_block(sn.as.repeat_stmt.body_block))
                    return true;
            }
            if (sn.type == NodeType::DoStatement) {
                if (recurse_block(sn.as.do_stmt.body_block))
                    return true;
            }
            if (sn.type == NodeType::ForStatement) {
                if (recurse_block(sn.as.for_stmt.body_block))
                    return true;
            }
            if (sn.type == NodeType::GenericForStatement) {
                if (recurse_block(sn.as.generic_for.body_block))
                    return true;
            }
            if (sn.type == NodeType::IfStatement) {
                if (recurse_block(sn.as.if_stmt.then_block))
                    return true;
                if (sn.as.if_stmt.else_block != 0xFFFFFFFF && recurse_block(sn.as.if_stmt.else_block))
                    return true;
            }
            if (sn.type == NodeType::FunctionDef) {
                if (sn.as.func_def.body_block != 0xFFFFFFFF && recurse_block(sn.as.func_def.body_block))
                    return true;
            }
        }
        return false;
    };
    return walk_block(walk_block, block_idx);
}

//------------------ CodeEmitter: constructor initializes output stream and binds analysis results
CodeEmitter::CodeEmitter(const ASTContext &context, const char *output_path, AnalysisState &analysis)
    : ctx(context)
    , out(output_path)
    , state(analysis) {
    out << std::setprecision(17);
}

//------------------ is_local: checks if variable is local in current scope
bool CodeEmitter::is_local(std::string_view name, bool &out_is_boxed) {
    std::string_view dummy;
    return is_local(name, out_is_boxed, dummy);
}

bool CodeEmitter::is_local(std::string_view name, bool &out_is_boxed, std::string_view &out_cpp_name) {
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (it->name == name) {
            out_is_boxed = it->is_boxed;
            out_cpp_name = it->cpp_name.empty() ? it->name : std::string_view(it->cpp_name);
            return true;
        }
    }
    return false;
}

//------------------ int_flag_expr: C++ bool expression for the runtime integer subtype of an expression
std::string CodeEmitter::int_flag_expr(uint32_t expr_idx, int depth) {
    if (depth > 24 || expr_idx == 0xFFFFFFFF || expr_idx >= ctx.nodes.size())
        return "false";
    const auto &n = ctx.nodes[expr_idx];
    if (n.type == NodeType::Integer)
        return "true";
    if (n.type == NodeType::ParenExpression)
        return int_flag_expr(n.as.paren_expr.expr, depth + 1);
    if (n.type == NodeType::Identifier && !n.as.ident.is_global) {
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        bool boxed = false;
        std::string_view cpp;
        if (is_local(nm, boxed, cpp)) {
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                if (it->name == nm) {
                    if (it->is_int_counter)
                        return "true";
                    if (!it->has_intf)
                        return "false";
                    std::string cn = it->cpp_name.empty() ? std::string(nm) : it->cpp_name;
                    return "_intf_l_" + cn;
                }
            }
            return "false";
        }
        if (state.for_counter_names.count(nm))
            return "true";
        return "false";
    }
    if (n.type == NodeType::BinaryOp) {
        int bop = n.as.bin_op.op;
        bool int_op = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
            || bop == static_cast<int>(BinaryOp::FloorDiv) || bop == static_cast<int>(BinaryOp::Mod);
        if (!int_op)
            return "false";
        return "(" + int_flag_expr(n.as.bin_op.left, depth + 1) + " && " + int_flag_expr(n.as.bin_op.right, depth + 1)
            + ")";
    }
    if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Minus))
        return int_flag_expr(n.as.unary_op.expr, depth + 1);
    if (n.type == NodeType::CallExpression) {
        uint32_t tgt = n.as.call_expr.target;
        if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier || ctx.nodes[tgt].as.ident.is_global)
            return "false";
        std::string_view callee(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
        if (!state.int_preserving_masks.count(callee))
            return "false";
        return fast_call_box_flag(callee, n.as.call_expr.first_arg, n.as.call_expr.arg_count);
    }
    return "false";
}

//------------------ fast_call_box_flag: C++ bool expression AND-ing each masked parameter's runtime

std::string CodeEmitter::fast_call_box_flag(std::string_view fname, uint32_t first_arg, uint32_t arg_count) {
    uint32_t mask = state.int_preserving_masks.at(fname);
    size_t total_params = state.func_param_counts.count(fname) ? state.func_param_counts.at(fname) : arg_count;
    if (mask == 0)
        return "false";
    std::string r = "(";
    bool any = false;
    for (size_t p = 0; p < total_params && p < 32; ++p) {
        if (!(mask & (1u << p)))
            continue;
        std::string f;
        if (p < arg_count) {
            f = int_flag_expr(ctx.block_statements[first_arg + p], 1);
        } else {
            f = "false";
        }
        r += (any ? " && " : "") + f;
        any = true;
    }
    r += any ? ")" : "false)";
    return r;
}

//------------------ int_value_expr: C++ int64 expression for the exact integer value of an expression

std::string CodeEmitter::int_value_expr(uint32_t expr_idx, int depth) {
    if (depth > 24 || expr_idx == 0xFFFFFFFF || expr_idx >= ctx.nodes.size())
        return "0";
    const auto &n = ctx.nodes[expr_idx];
    if (n.type == NodeType::Integer)
        return "static_cast<int64_t>(" + std::to_string(n.as.integer.val) + ")";
    if (n.type == NodeType::ParenExpression)
        return int_value_expr(n.as.paren_expr.expr, depth + 1);
    if (n.type == NodeType::Identifier && !n.as.ident.is_global) {
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        bool boxed = false;
        std::string_view cpp;
        if (is_local(nm, boxed, cpp)) {
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                if (it->name == nm) {
                    std::string cn = it->cpp_name.empty() ? std::string(nm) : it->cpp_name;
                    if (it->is_int_counter || state.native_integers.count(std::string(nm))) {
                        if (boxed)
                            return "(*l_" + cn + ").as_integer()";
                        return "static_cast<int64_t>(l_" + cn + ")";
                    }
                    if (it->has_intf)
                        return "_ii_l_" + cn;
                    return "0";
                }
            }
            return "0";
        }
        if (state.for_counter_names.count(nm))
            return "static_cast<int64_t>(l_" + std::string(nm) + ")";
        return "0";
    }
    if (n.type == NodeType::BinaryOp) {
        int bop = n.as.bin_op.op;
        const char *fn = bop == static_cast<int>(BinaryOp::Add) ? "clx::int_add"
            : bop == static_cast<int>(BinaryOp::Sub)            ? "clx::int_sub"
            : bop == static_cast<int>(BinaryOp::Mul)            ? "clx::int_mul"
            : bop == static_cast<int>(BinaryOp::FloorDiv)       ? "clx::int_floor_div"
            : bop == static_cast<int>(BinaryOp::Mod)            ? "clx::int_floor_mod"
                                                                : nullptr;
        if (!fn)
            return "0";
        return std::string(fn) + "(" + int_value_expr(n.as.bin_op.left, depth + 1) + ", "
            + int_value_expr(n.as.bin_op.right, depth + 1) + ")";
    }
    if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Minus))
        return "clx::int_neg(" + int_value_expr(n.as.unary_op.expr, depth + 1) + ")";
    if (n.type == NodeType::CallExpression) {
        uint32_t tgt = n.as.call_expr.target;
        if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier || ctx.nodes[tgt].as.ident.is_global)
            return "0";
        std::string_view callee(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
        if (!state.int_preserving_masks.count(callee))
            return "0";

        uint32_t mask = state.int_preserving_masks.at(callee);
        size_t total_params
            = state.func_param_counts.count(callee) ? state.func_param_counts.at(callee) : n.as.call_expr.arg_count;
        std::string r = "(";
        bool any = false;
        for (size_t p = 0; p < total_params && p < 32; ++p) {
            if (!(mask & (1u << p)))
                continue;
            if (p < n.as.call_expr.arg_count) {
                uint32_t a_idx = ctx.block_statements[n.as.call_expr.first_arg + p];
                if (ctx.nodes[a_idx].type == NodeType::Identifier && !ctx.nodes[a_idx].as.ident.is_global) {
                    std::string f = int_value_expr(a_idx, 1);
                    r += (any ? " && " : "") + f;
                    any = true;
                    continue;
                }
            }
            return "0";
        }
        return any ? r + ")" : "0";
    }
    return "0";
}

//------------------ emit: generates C++ code for the AST rooted at root_node
void CodeEmitter::emit(uint32_t root_node, std::string_view module_name) {
    state.native_numbers.clear();
    state.string_pool.clear();
    state.table_presize.clear();
    state.global_constants.clear();
    state.bce_safe_nodes.clear();
    state.direct_callables.clear();
    state.fast_callables.clear();
    state.native_return_funcs.clear();
    state.int_returning_funcs.clear();
    state.int_typed_locals.clear();
    state.table_typed_locals.clear();
    state.func_param_counts.clear();
    state.param_numbers.clear();
    state.param_names.clear();
    state.func_param_native.clear();
    state.reassigned_vars.clear();
    state.constant_upvalues.clear();
    state.const_snapshot_cells.clear();
    state.string_builders.clear();
    state.global_string_builders.clear();
    state.module_string_builders.clear();
    state.goto_targets.clear();
    state.hoisted_locals.clear();
    state.pure_numeric_arrays.clear();
    state.numeric_table_fields.clear();
    state.native_integers.clear();
    state.emit_raw_lambda = false;
    state.emit_fast_lambda = false;
    state.in_fast_function = false;
    state.in_function_def = false;
    state.in_native_impl = false;
    state.current_fast_func = "";
    state.current_native_func = "";
    state.native_emitted.clear();
    state.hoisted_tables.clear();
    state.ref_capture.clear();
    state.raw_lambda_cell.clear();
    state.holder_cell_callables.clear();

    Optimizer(ctx, state).run(ctx, root_node);

    std::set<std::string_view> captured_decl_names;
    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto &nd = ctx.nodes[i];
        if (nd.type == NodeType::LocalDecl) {
            for (uint32_t ii = 0; ii < nd.as.local_decl.ident_count; ++ii) {
                uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + ii];
                if (ctx.nodes[idi].type == NodeType::Identifier && ctx.nodes[idi].as.ident.is_captured) {
                    captured_decl_names.insert(
                        std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length));
                }
            }
        } else if (nd.type == NodeType::FunctionDef) {
            for (uint32_t pi = 0; pi < nd.as.func_def.param_count; ++pi) {
                uint32_t pidx = ctx.block_statements[nd.as.func_def.first_param + pi];
                if (ctx.nodes[pidx].type == NodeType::Identifier && ctx.nodes[pidx].as.ident.is_captured) {
                    captured_decl_names.insert(
                        std::string_view(ctx.nodes[pidx].as.ident.name, ctx.nodes[pidx].as.ident.length));
                }
            }
            if (nd.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
                uint32_t vidx = nd.as.func_def.named_vararg_ident;
                if (ctx.nodes[vidx].type == NodeType::Identifier && ctx.nodes[vidx].as.ident.is_captured) {
                    captured_decl_names.insert(
                        std::string_view(ctx.nodes[vidx].as.ident.name, ctx.nodes[vidx].as.ident.length));
                }
            }
        } else if (nd.type == NodeType::ForStatement) {
            uint32_t vidx = nd.as.for_stmt.var_ident;
            if (vidx < ctx.nodes.size() && ctx.nodes[vidx].type == NodeType::Identifier
                && ctx.nodes[vidx].as.ident.is_captured) {
                captured_decl_names.insert(
                    std::string_view(ctx.nodes[vidx].as.ident.name, ctx.nodes[vidx].as.ident.length));
            }
        } else if (nd.type == NodeType::GenericForStatement) {
            for (uint32_t vi = 0; vi < nd.as.generic_for.var_count; ++vi) {
                uint32_t vidx = ctx.block_statements[nd.as.generic_for.first_var + vi];
                if (vidx < ctx.nodes.size() && ctx.nodes[vidx].type == NodeType::Identifier
                    && ctx.nodes[vidx].as.ident.is_captured) {
                    captured_decl_names.insert(
                        std::string_view(ctx.nodes[vidx].as.ident.name, ctx.nodes[vidx].as.ident.length));
                }
            }
        }
    }

    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto &node = ctx.nodes[i];
        bool is_module_level = false;
        if (root_node < ctx.nodes.size() && ctx.nodes[root_node].type == NodeType::Block) {
            const auto &rb = ctx.nodes[root_node].as.block;
            for (uint32_t j = 0; j < rb.count; ++j) {
                if (ctx.block_statements[rb.first_statement + j] == i) {
                    is_module_level = true;
                    break;
                }
            }
        }
        if (node.type == NodeType::Assignment && node.as.assign.target_count == 1 && node.as.assign.value_count == 1) {
            uint32_t t_idx = ctx.block_statements[node.as.assign.first_target];
            uint32_t v_idx = ctx.block_statements[node.as.assign.first_value];
            if (ctx.nodes[t_idx].type == NodeType::Identifier) {
                std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                const auto &v_node = ctx.nodes[v_idx];
                if (v_node.type == NodeType::BinaryOp && v_node.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {

                    std::vector<uint32_t> ops;
                    std::vector<uint32_t> ws;
                    ws.push_back(v_idx);
                    while (!ws.empty()) {
                        uint32_t cur = ws.back();
                        ws.pop_back();
                        const auto &cn = ctx.nodes[cur];
                        if (cn.type == NodeType::BinaryOp && cn.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                            ws.push_back(cn.as.bin_op.right);
                            ws.push_back(cn.as.bin_op.left);
                        } else {
                            ops.push_back(cur);
                        }
                    }
                    if (!ops.empty() && ctx.nodes[ops[0]].type == NodeType::Identifier) {
                        std::string_view fn(ctx.nodes[ops[0]].as.ident.name, ctx.nodes[ops[0]].as.ident.length);
                        if (fn == name && !captured_decl_names.count(name)) {
                            state.string_builders.insert(name);
                            if (ctx.nodes[t_idx].as.ident.is_global) {
                                state.global_string_builders.insert(name);
                            } else if (is_module_level) {
                                state.module_string_builders.insert(name);
                            }
                        }
                    }
                }
            }
        }
    }

    {
        std::set<std::string_view> concat_idents;
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &node = ctx.nodes[i];
            if (node.type != NodeType::BinaryOp || node.as.bin_op.op != 13)
                continue;
            std::vector<uint32_t> ws;
            std::vector<uint32_t> ops;
            ws.push_back(i);
            while (!ws.empty()) {
                uint32_t cur = ws.back();
                ws.pop_back();
                const auto &cn = ctx.nodes[cur];
                if (cn.type == NodeType::BinaryOp && cn.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                    ws.push_back(cn.as.bin_op.right);
                    ws.push_back(cn.as.bin_op.left);
                } else {
                    ops.push_back(cur);
                }
            }
            for (auto op : ops) {
                if (ctx.nodes[op].type == NodeType::Identifier && !ctx.nodes[op].as.ident.is_global) {
                    std::string_view nm(ctx.nodes[op].as.ident.name, ctx.nodes[op].as.ident.length);
                    concat_idents.insert(nm);
                }
            }
        }
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &node = ctx.nodes[i];
            if (node.type != NodeType::LocalDecl)
                continue;
            bool is_module_level = false;
            if (root_node < ctx.nodes.size() && ctx.nodes[root_node].type == NodeType::Block) {
                const auto &rb = ctx.nodes[root_node].as.block;
                for (uint32_t j = 0; j < rb.count; ++j) {
                    if (ctx.block_statements[rb.first_statement + j] == i) {
                        is_module_level = true;
                        break;
                    }
                }
            }
            if (!is_module_level)
                continue;
            for (uint32_t ii = 0; ii < node.as.local_decl.ident_count; ++ii) {
                uint32_t idi = ctx.block_statements[node.as.local_decl.first_ident + ii];
                if (ctx.nodes[idi].type != NodeType::Identifier)
                    continue;
                std::string_view nm(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                if (concat_idents.count(nm) && !ctx.nodes[idi].as.ident.is_captured) {
                    state.string_builders.insert(nm);
                    state.module_string_builders.insert(nm);
                }
            }
        }
    }

    out << "#include <clx.h>\n";
    out << "#include <atomic>\n\n";

    if (!state.string_pool.empty()) {
        out << "static clx::LValue cstr_[" << state.string_pool.size() << "];\n";
    }
    out << "static inline clx::LValue clx_gettable_safe(clx::LValue val) { return val; }\n\n";

    out << "CLX_API clx::LValue luaopen_" << module_name << "(clx::LState* L) {\n";
    out << "    clx::LValue _ENV(clx::ValueType::Table, L->_G);\n";

    for (const auto &sb_name : state.global_string_builders) {
        out << "    clx::StringBuilder sb_" << sb_name << ";\n";
    }
    for (const auto &sb_name : state.module_string_builders) {
        out << "    clx::StringBuilder sb_" << sb_name << ";\n";
    }

    if (!state.string_pool.empty()) {
        size_t n = state.string_pool.size();
        size_t cap = 64;
        while (cap < n * 2)
            cap *= 2;
        out << "    L->string_pool.reserve(" << cap << ");\n";

        std::vector<uint8_t> occupied(cap, 0);
        std::vector<size_t> slot_assignments(n);
        size_t mask = cap - 1;

        for (size_t i = 0; i < n; ++i) {
            auto &s = state.string_pool[i];
            std::string decoded(s);
            uint64_t h = (decoded.length() <= 8) ? swar_hash_8(decoded.data(), decoded.length())
                                                 : wyhash_str(decoded.data(), decoded.length());
            size_t idx = h & mask;
            while (occupied[idx])
                idx = (idx + 1) & mask;
            occupied[idx] = 1;
            slot_assignments[i] = idx;
        }

        out << "    static const clx::StringPool::PrecomputedEntry _cstr_all[" << n << "] = {\n";
        for (size_t i = 0; i < n; ++i) {
            auto &s = state.string_pool[i];
            std::string decoded(s);
            uint64_t h = (decoded.length() <= 8) ? swar_hash_8(decoded.data(), decoded.length())
                                                 : wyhash_str(decoded.data(), decoded.length());
            out << "        {\"" << cpp_escape(decoded) << "\", " << (unsigned int)decoded.length() << ", " << h
                << "ULL, " << (unsigned int)slot_assignments[i] << "},\n";
        }
        out << "    };\n";
        out << "    L->string_pool.bulk_fill_precomputed(_cstr_all, " << n << ");\n";

        for (size_t i = 0; i < n; ++i) {
            out << "    cstr_[" << i << "] = clx::LValue(L->string_pool.slots[" << slot_assignments[i] << "].baked);\n";
        }
    }

    if (!ctx.nodes.empty()) {
        state.current_func_body = root_node;
        emit_node(root_node);
    }
    out << "    return clx::LValue();\n";
    out << "}\n";
}

//------------------ emit_node: dispatches a single AST node to its emitXxx method
void CodeEmitter::emit_node(uint32_t node_idx) {
    if (node_idx >= ctx.nodes.size())
        return;
    const ASTNode &node = ctx.nodes[node_idx];

    switch (node.type) {
    case NodeType::IntrinsicCall:
        emitIntrinsicCall(node, node_idx);
        break;
    case NodeType::CallExpression:
        emitCallExpression(node, node_idx);
        break;
    case NodeType::ParenExpression:
        emitParenExpression(node, node_idx);
        break;
    case NodeType::LabelStatement:
        emitLabelStatement(node, node_idx);
        break;
    case NodeType::GotoStatement:
        emitGotoStatement(node, node_idx);
        break;
    case NodeType::Block:
        emitBlock(node, node_idx);
        break;
    case NodeType::FunctionDef:
        emitFunctionDef(node, node_idx);
        break;
    case NodeType::ReturnStatement:
        emitReturnStatement(node, node_idx);
        break;
    case NodeType::GlobalDeclStatement:
    case NodeType::LocalDecl:
    case NodeType::Assignment:
        emitAssignmentLike(node, node_idx);
        break;
    case NodeType::DoStatement:
        emitDoStatement(node, node_idx);
        break;
    case NodeType::UnaryOp:
        emitUnaryOp(node, node_idx);
        break;
    case NodeType::BinaryOp:
        emitBinaryOp(node, node_idx);
        break;
    case NodeType::TrueLiteral:
        emitTrueLiteral(node, node_idx);
        break;
    case NodeType::FalseLiteral:
        emitFalseLiteral(node, node_idx);
        break;
    case NodeType::NilLiteral:
        emitNilLiteral(node, node_idx);
        break;
    case NodeType::Number:
        emitNumber(node, node_idx);
        break;
    case NodeType::Integer:
        emitInteger(node, node_idx);
        break;
    case NodeType::Identifier:
        emitIdentifier(node, node_idx);
        break;
    case NodeType::String:
        emitString(node, node_idx);
        break;
    case NodeType::IfStatement:
        emitIfStatement(node, node_idx);
        break;
    case NodeType::WhileStatement:
        emitWhileStatement(node, node_idx);
        break;
    case NodeType::RepeatStatement:
        emitRepeatStatement(node, node_idx);
        break;
    case NodeType::ForStatement:
        emitForStatement(node, node_idx);
        break;
    case NodeType::GenericForStatement:
        emitGenericForStatement(node, node_idx);
        break;
    case NodeType::TableConstructor:
        emitTableConstructor(node, node_idx);
        break;
    case NodeType::TableAccess:
        emitTableAccess(node, node_idx);
        break;
    case NodeType::Vararg:
        emitVararg(node, node_idx);
        break;
    case NodeType::BreakStatement:
        emitBreakStatement(node, node_idx);
        break;
    }
}

}
