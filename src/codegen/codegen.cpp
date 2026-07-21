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
#include "../../include/clx.h"
#include "../optimizer/optimizer.h"
#include <set>
#include <map>
#include <algorithm>
#include <vector>
#include <cstring>
#include <iomanip>

namespace clx {

//------------------ lookup_builtin: maps "module.func" to C++ function name
const char *lookup_builtin(std::string_view module, std::string_view func) {
    static const std::unordered_map<std::string_view, std::unordered_map<std::string_view, const char *>> _sfm
        = { { "string",
                { { "byte", "str_byte" }, { "sub", "str_sub" }, { "match", "str_match" }, { "find", "str_find" },
                    { "gsub", "str_gsub" }, { "len", "str_len" }, { "format", "str_format" }, { "char", "str_char" },
                    { "rep", "str_rep" }, { "reverse", "str_reverse" }, { "lower", "str_lower" },
                    { "upper", "str_upper" }, { "dump", "str_dump" } } },
              { "table",
                  { { "concat", "table_concat" }, { "insert", "table_insert" }, { "remove", "table_remove" },
                      { "sort", "table_sort" }, { "pack", "table_pack" }, { "unpack", "table_unpack" },
                      { "move", "table_move" } } },
              { "_G", { { "type", "__clx_type" }, { "tostring", "__clx_tostring" } } } };
    auto _smit = _sfm.find(module);
    if (_smit != _sfm.end()) {
        auto _sfit = _smit->second.find(func);
        if (_sfit != _smit->second.end())
            return _sfit->second;
    }
    return nullptr;
}

//------------------ collect_string_builder_refs: walk an argument subtree and
void collect_string_builder_refs(
    const ASTContext &ctx, uint32_t n_idx, const std::set<std::string_view> &sb_set, std::set<std::string_view> &out) {
    if (n_idx == 0xFFFFFFFF || n_idx >= ctx.nodes.size())
        return;
    const auto &nd = ctx.nodes[n_idx];
    if (nd.type == NodeType::Identifier) {
        std::string_view nm(nd.as.ident.name, nd.as.ident.length);
        if (sb_set.count(nm))
            out.insert(nm);
        return;
    }
    if (nd.type == NodeType::BinaryOp) {
        collect_string_builder_refs(ctx, nd.as.bin_op.left, sb_set, out);
        collect_string_builder_refs(ctx, nd.as.bin_op.right, sb_set, out);
        return;
    }
    if (nd.type == NodeType::UnaryOp) {
        collect_string_builder_refs(ctx, nd.as.unary_op.expr, sb_set, out);
        return;
    }
    if (nd.type == NodeType::ParenExpression) {
        collect_string_builder_refs(ctx, nd.as.paren_expr.expr, sb_set, out);
        return;
    }
    if (nd.type == NodeType::TableAccess) {
        collect_string_builder_refs(ctx, nd.as.table_access.table, sb_set, out);
        collect_string_builder_refs(ctx, nd.as.table_access.key, sb_set, out);
        return;
    }
    if (nd.type == NodeType::CallExpression) {
        collect_string_builder_refs(ctx, nd.as.call_expr.target, sb_set, out);
        for (uint32_t j = 0; j < nd.as.call_expr.arg_count; ++j) {
            collect_string_builder_refs(ctx, ctx.block_statements[nd.as.call_expr.first_arg + j], sb_set, out);
        }
        return;
    }
}

//------------------ var_reassigned_non_int: checks if a variable receives any non-integer value in a block tree
bool CodeEmitter::var_reassigned_non_int(std::string_view name, uint32_t block_idx) {

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
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (it->name == name) {
            out_is_boxed = it->is_boxed;
            return true;
        }
    }
    return false;
}

static std::string lua_decode_string(std::string_view s) {
    std::string r;
    r.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\\' && i + 1 < s.length()) {
            unsigned char c = static_cast<unsigned char>(s[i + 1]);
            switch (c) {
            case 'a':
                r += '\a';
                i++;
                break;
            case 'b':
                r += '\b';
                i++;
                break;
            case 'f':
                r += '\f';
                i++;
                break;
            case 'n':
                r += '\n';
                i++;
                break;
            case 'r':
                r += '\r';
                i++;
                break;
            case 't':
                r += '\t';
                i++;
                break;
            case 'v':
                r += '\v';
                i++;
                break;
            case '\\':
                r += '\\';
                i++;
                break;
            case '"':
                r += '\"';
                i++;
                break;
            case '\'':
                r += '\'';
                i++;
                break;
            case 'x': {
                if (i + 3 < s.length()) {
                    char hex[3] = { s[i + 2], s[i + 3], 0 };
                    char *end;
                    long val = std::strtol(hex, &end, 16);
                    if (end == hex + 2) {
                        r += static_cast<char>(val);
                        i += 3;
                    } else {
                        r += s[i];
                    }
                } else {
                    r += s[i];
                }
                break;
            }
            case 'u': {
                if (i + 2 < s.length() && s[i + 2] == '{') {
                    size_t end = s.find('}', i + 3);
                    if (end != std::string_view::npos) {
                        std::string hx(s.data() + i + 3, end - i - 3);
                        unsigned long cp = std::strtoul(hx.c_str(), nullptr, 16);
                        if (cp < 0x80) {
                            r += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            r += static_cast<char>(0xC0 | (cp >> 6));
                            r += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            r += static_cast<char>(0xE0 | (cp >> 12));
                            r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            r += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            r += static_cast<char>(0xF0 | (cp >> 18));
                            r += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            r += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i = end;
                    } else {
                        r += s[i];
                    }
                } else {
                    r += s[i];
                }
                break;
            }
            default: {

                if (c >= '0' && c <= '9') {
                    size_t j = i + 1;
                    while (j < s.length() && j - i <= 3 && s[j] >= '0' && s[j] <= '7')
                        j++;
                    if (j > i + 1) {
                        std::string oct(s.data() + i + 1, j - i - 1);
                        long val = std::strtol(oct.c_str(), nullptr, 8);
                        r += static_cast<char>(val);
                        i = j - 1;
                    } else {
                        r += s[i];
                    }
                } else {
                    r += s[i];
                }
                break;
            }
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

static std::string cpp_escape(std::string_view s) {
    std::string r;
    r.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':
            r += "\\\"";
            break;
        case '\\':
            r += "\\\\";
            break;
        case '\a':
            r += "\\a";
            break;
        case '\b':
            r += "\\b";
            break;
        case '\f':
            r += "\\f";
            break;
        case '\n':
            r += "\\n";
            break;
        case '\r':
            r += "\\r";
            break;
        case '\t':
            r += "\\t";
            break;
        case '\v':
            r += "\\v";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                bool next_is_hex = (i + 1 < s.length()
                    && ((s[i + 1] >= '0' && s[i + 1] <= '9') || (s[i + 1] >= 'a' && s[i + 1] <= 'f')
                        || (s[i + 1] >= 'A' && s[i + 1] <= 'F')));
                if (next_is_hex) {
                    std::snprintf(buf, sizeof(buf), "\\%03o", c);
                } else {
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                }
                r += buf;
            } else {
                r += c;
            }
        }
    }
    return r;
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
    state.func_param_counts.clear();
    state.param_numbers.clear();
    state.param_names.clear();
    state.func_param_native.clear();
    state.reassigned_vars.clear();
    state.constant_upvalues.clear();
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
    state.current_fast_func = "";
    state.ref_capture.clear();

    Optimizer(ctx, state).run(ctx, root_node);

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
                        if (fn == name) {
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
        size_t long_count = 0;

        for (size_t i = 0; i < n; ++i) {
            std::string decoded = lua_decode_string(state.string_pool[i]);
            if (decoded.length() > 6)
                long_count++;
        }

        if (long_count > 0) {
            size_t cap = 64;
            while (cap < long_count * 2)
                cap *= 2;
            out << "    L->string_pool.reserve(" << cap << ");\n";
        }

        for (size_t i = 0; i < n; ++i) {
            auto &s = state.string_pool[i];
            std::string decoded = lua_decode_string(s);
            if (decoded.length() <= 6) {
                out << "    cstr_[" << i << "] = clx::LValue::istr(\"" << cpp_escape(decoded) << "\", "
                    << decoded.length() << ");\n";
            }
        }

        if (long_count > 0) {
            size_t cap = 64;
            while (cap < long_count * 2)
                cap *= 2;
            std::vector<uint8_t> occupied(cap, 0);
            std::vector<size_t> slot_assignments;
            size_t mask = cap - 1;

            for (size_t i = 0; i < n; ++i) {
                auto &s = state.string_pool[i];
                std::string decoded = lua_decode_string(s);
                if (decoded.length() > 6) {
                    uint64_t h = (decoded.length() <= 8) ? swar_hash_8(decoded.data(), decoded.length())
                                                         : wyhash_str(decoded.data(), decoded.length());
                    size_t idx = h & mask;
                    while (occupied[idx])
                        idx = (idx + 1) & mask;
                    occupied[idx] = 1;
                    slot_assignments.push_back(idx);
                }
            }

            out << "    static const clx::StringPool::PrecomputedEntry _cstr_long[" << long_count << "] = {\n";
            size_t li = 0;
            for (size_t i = 0; i < n; ++i) {
                auto &s = state.string_pool[i];
                std::string decoded = lua_decode_string(s);
                if (decoded.length() > 6) {
                    uint64_t h = (decoded.length() <= 8) ? swar_hash_8(decoded.data(), decoded.length())
                                                         : wyhash_str(decoded.data(), decoded.length());
                    out << "        {\"" << cpp_escape(decoded) << "\", " << (unsigned int)decoded.length() << ", " << h
                        << "ULL, " << (unsigned int)slot_assignments[li] << "},\n";
                    li++;
                }
            }
            out << "    };\n";
            out << "    L->string_pool.bulk_fill_precomputed(_cstr_long, " << long_count << ");\n";

            li = 0;
            for (size_t i = 0; i < n; ++i) {
                auto &s = state.string_pool[i];
                std::string decoded = lua_decode_string(s);
                if (decoded.length() > 6) {
                    out << "    cstr_[" << i << "] = clx::LValue(L->string_pool.slots[" << slot_assignments[li]
                        << "].baked);\n";
                    li++;
                }
            }
        }
    }

    if (!ctx.nodes.empty()) {
        state.current_func_body = root_node;
        emit_node(root_node);
    }
    out << "    return clx::LValue();\n";
    out << "}\n";
}

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
            out << n.as.number.val;
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
                out << "static_cast<size_t>(l_" << name << ".as_integer())";
                return;
            }
            if (std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                != state.native_numbers.end()) {
                bool is_boxed = false;
                this->is_local(name, is_boxed);
                if (is_boxed)
                    out << "(*l_" << name << ").as_number()";
                else
                    out << "l_" << name;
                return;
            }
        }
        if (n.type == NodeType::BinaryOp) {
            int op = n.as.bin_op.op;
            if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Mul)
                && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.right)) {
                out << "static_cast<int64_t>(";
                emit_native(n.as.bin_op.left);
                out << ")";
                if (op == 1)
                    out << " + ";
                if (op == 2)
                    out << " - ";
                if (op == 3)
                    out << " * ";
                out << "static_cast<int64_t>(";
                emit_native(n.as.bin_op.right);
                out << ")";
                return;
            }
            if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) {
                if (op == static_cast<int>(BinaryOp::Div)) {
                    out << "(static_cast<double>(";
                    emit_native(n.as.bin_op.left);
                    out << ") / static_cast<double>(";
                    emit_native(n.as.bin_op.right);
                    out << "))";
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
                out << "std::floor((";
                emit_native(n.as.bin_op.left);
                out << ") / (";
                emit_native(n.as.bin_op.right);
                out << "))";
                return;
            }
            if (op == static_cast<int>(BinaryOp::Mod)) {
                if (clx::is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                    && clx::is_purely_integer_expr(ctx, state, n.as.bin_op.right)) {
                    out << "static_cast<int64_t>(static_cast<int64_t>(";
                    emit_native(n.as.bin_op.left);
                    out << ") % static_cast<int64_t>(";
                    emit_native(n.as.bin_op.right);
                    out << "))";
                } else {
                    out << "std::fmod(";
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
                    auto it = state.numeric_table_fields.find(t_name);
                    if (it != state.numeric_table_fields.end()) {
                        std::string_view fn(ctx.nodes[n.as.table_access.key].as.string.text,
                            ctx.nodes[n.as.table_access.key].as.string.length);
                        if (it->second.count(fn)) {
                            size_t idx = std::distance(state.string_pool.begin(),
                                std::find(state.string_pool.begin(), state.string_pool.end(), fn));
                            out << "clx::table_get(L, ";
                            emit_node(n.as.table_access.table);
                            out << ", cstr_[" << idx << "]).as_number()";
                            return;
                        }
                    }
                }
                return;
            }
            if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
                out << "l_" << t_name << "[static_cast<size_t>(";
                emit_native(n.as.table_access.key);
                out << ") - 1]";
                return;
            }
            if (state.bce_safe_nodes.count(n_idx)) {
                out << "([&](){ auto* _t" << n_idx << " = static_cast<clx::LTable*>((";
                emit_node(n.as.table_access.table);
                out << ").as_pointer()); size_t _k" << n_idx << " = static_cast<size_t>(";
                emit_native(n.as.table_access.key);
                out << "); return clx::LValue(_t" << n_idx << "->array[_k" << n_idx << " - 1], _t" << n_idx
                    << "->array_types[_k" << n_idx << " - 1]).as_number(); }())";
                return;
            }
            bool pure_t = ctx.nodes[n.as.table_access.table].type == NodeType::Identifier;
            if (pure_t) {
                out << "([&](){ size_t _idx" << n_idx << " = static_cast<size_t>(";
                emit_native(n.as.table_access.key);
                out << ") - 1; auto* _t" << n_idx << " = static_cast<clx::LTable*>((";
                emit_node(n.as.table_access.table);
                out << ").as_pointer()); return (_idx" << n_idx << " < _t" << n_idx << "->array_size) ? clx::LValue(_t"
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
            out << "(l_" << t_name << "[static_cast<size_t>(";
            emit_native(c.as.table_access.key);
            out << ") - 1] != 0.0)";
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
                out << "(";
                emit_native(c.as.bin_op.left);
                out << ops[op];
                emit_native(c.as.bin_op.right);
                out << ")";
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
        out << "(";
        emit_condition(c.as.paren_expr.expr);
        out << ")";
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
        out << ")))";
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
            out << "    _m_func = ([&](){ clx::LTable* _t = static_cast<clx::LTable*>(_m_self.as_pointer()); size_t _k "
                   "= static_cast<size_t>(";
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

        if (is_direct) {
            out << "    clx::MultiValue _main_ret = _impl_" << fname << "(L, _dyn_buf, _dyn_count);\n";
        } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
            && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
            auto _chit = state.hoisted_lookups.find(node.as.call_expr.target);
            if (_chit != state.hoisted_lookups.end()) {
                auto _cf_it = state.hoisted_cfuncs.find(_chit->second);
                if (_cf_it != state.hoisted_cfuncs.end()) {
                    out << "    for (size_t i = 0; i < _dyn_count; ++i) L->shadow_stack[L->shadow_top++] = "
                           "clx::TypedSlot(&_dyn_buf[i].val, &_dyn_buf[i].type);\n";
                    out << "    clx::MultiValue _main_ret = clx::" << _cf_it->second << "(L, _dyn_buf, _dyn_count);\n";
                    out << "    L->shadow_top -= _dyn_count;\n";
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
                out << "    for (size_t i = 0; i < _dyn_count; ++i) L->shadow_stack[L->shadow_top++] = "
                       "clx::TypedSlot(&_dyn_buf[i].val, &_dyn_buf[i].type);\n";
                out << "    clx::MultiValue _main_ret = clx::" << _alias_it->second << "(L, _dyn_buf, _dyn_count);\n";
                out << "    L->shadow_top -= _dyn_count;\n";
            } else {
                goto _call_direct_dyn;
            }
        } else {
        _call_direct_dyn:;
            out << "    for (size_t i = 0; i < _dyn_count; ++i) L->shadow_stack[L->shadow_top++] = "
                   "clx::TypedSlot(&_dyn_buf[i].val, &_dyn_buf[i].type);\n";
            out << "    clx::MultiValue _main_ret = clx::call_direct(L, ";
            if (is_method_call)
                out << "_m_func";
            else
                emit_node(node.as.call_expr.target);
            out << ", _dyn_buf, _dyn_count, \"" << ctx.filename << "\", " << node.line << ");\n";
            out << "    L->shadow_top -= _dyn_count;\n";
        }

        if (is_method_call)
            out << "    L->shadow_top -= 2;\n";

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
            if (!state.global_string_builders.count(sb_name) && !state.module_string_builders.count(sb_name)) {
                out << "    clx::StringBuilder sb_" << sb_name << ";\n";
            }
        }
        if (node.as.call_expr.arg_count > 0) {
            out << "    clx::LValue args[] = {";
            for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i) {
                if (is_method_call && i == 0)
                    out << "_m_self";
                else
                    emit_node(ctx.block_statements[node.as.call_expr.first_arg + i]);
                if (i < node.as.call_expr.arg_count - 1)
                    out << ", ";
            }
            out << "};\n";
            if (is_direct) {
                out << "    clx::MultiValue _main_ret = _impl_" << fname << "(L, args, " << node.as.call_expr.arg_count
                    << ");\n";
            } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size()
                && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
                auto _chit = state.hoisted_lookups.find(node.as.call_expr.target);
                if (_chit != state.hoisted_lookups.end()) {
                    auto _cf_it = state.hoisted_cfuncs.find(_chit->second);
                    if (_cf_it != state.hoisted_cfuncs.end()) {
                        out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count
                            << "; ++i) L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[i].val, "
                               "&args[i].type);\n";
                        out << "    clx::MultiValue _main_ret = clx::" << _cf_it->second << "(L, args, "
                            << node.as.call_expr.arg_count << ");\n";
                        out << "    L->shadow_top -= " << node.as.call_expr.arg_count << ";\n";
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
                    out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count
                        << "; ++i) L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[i].val, &args[i].type);\n";
                    out << "    clx::MultiValue _main_ret = clx::" << _alias_it->second << "(L, args, "
                        << node.as.call_expr.arg_count << ");\n";
                    out << "    L->shadow_top -= " << node.as.call_expr.arg_count << ";\n";
                } else {
                    goto _call_direct_normal;
                }
            } else {
            _call_direct_normal:
                out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count
                    << "; ++i) L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args[i].val, &args[i].type);\n";
                out << "    clx::MultiValue _main_ret = clx::call_direct(L, ";
                if (is_method_call)
                    out << "_m_func";
                else
                    emit_node(node.as.call_expr.target);
                out << ", args, " << node.as.call_expr.arg_count << ", \"" << ctx.filename << "\", " << node.line
                    << ");\n";
                out << "    L->shadow_top -= " << node.as.call_expr.arg_count << ";\n";
            }
        } else {
            if (is_direct) {
                out << "    clx::MultiValue _main_ret = _impl_" << fname << "(L, nullptr, 0);\n";
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

        if (is_method_call)
            out << "    L->shadow_top -= 2;\n";

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
void CodeEmitter::emitBlock(const ASTNode &node, uint32_t node_idx) {
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

//------------------ emitFunctionDef: handles NodeType::FunctionDef
void CodeEmitter::emitFunctionDef(const ASTNode &node, uint32_t node_idx) {
    bool is_raw = state.emit_raw_lambda;
    bool is_fast = state.emit_fast_lambda;
    state.emit_raw_lambda = false;
    state.emit_fast_lambda = false;
    bool prev_in_func = state.in_function_def;
    state.in_function_def = true;

    auto saved_direct_callables = state.direct_callables;
    auto saved_fast_callables = state.fast_callables;
    uint32_t saved_func_body = state.current_func_body;
    state.current_func_body = node.as.func_def.body_block;

    if (is_fast) {
        out << "[&](auto& self";
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
            std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
            out << ", double l_" << pname;
        }
        out << ") -> double {\n";

        size_t prev_locals = locals.size();
        size_t prev_native_count = state.native_numbers.size();
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
            std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
            locals.push_back({ pname, false });
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
        out << "return 0.0;\n}";
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
    out << "clx::ScopeGuard _sg_func(L);\n";
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

    size_t prev_native_count_for_params = state.native_numbers.size();

    for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
        uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
        bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
        bool is_native
            = std::find(state.native_numbers.begin(), state.native_numbers.end(), pname) != state.native_numbers.end()
            || (!is_cap && state.param_numbers[node_idx].count(std::string(pname)));

        if (is_cap) {
            if (state.constant_upvalues.count(pname)) {
                out << "clx::LValue l_" << pname << " = (" << i << " < arg_count) ? args[" << i
                    << "] : clx::LValue();\n";
                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << pname << ".val, &l_" << pname
                    << ".type);\n";
            } else {
                out << "clx::LUpValue l_" << pname << ";\n";
                out << "l_" << pname << " = clx::make_upvalue((" << i << " < arg_count) ? args[" << i
                    << "] : clx::LValue());\n";
                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << pname << "->val, &l_" << pname
                    << "->type);\n";
            }
        } else if (is_native) {
            out << "double l_" << pname << " = (" << i << " < arg_count) ? args[" << i << "].as_number() : 0.0;\n";
            if (std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                == state.native_numbers.end())
                state.native_numbers.push_back(pname);
        } else {
            out << "clx::LValue l_" << pname << " = (" << i << " < arg_count) ? args[" << i << "] : clx::LValue();\n";
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << pname << ".val, &l_" << pname
                << ".type);\n";
        }
        if (state.string_builders.count(pname) && !state.global_string_builders.count(pname)
            && !state.module_string_builders.count(pname)) {
            out << "clx::StringBuilder sb_" << pname << ";\n";
        }
    }

    if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
        std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name,
            ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
        out << "clx::LValue l_" << vaname << " = L->create_table(_va_count, 1);\n";
        out << "clx::LTable* _t_" << vaname << " = static_cast<clx::LTable*>(l_" << vaname << ".as_pointer());\n";
        out << "_t_" << vaname
            << "->settable(clx::LValue(L->intern_string(\"n\")), clx::LValue(static_cast<double>(_va_count)));\n";
        out << "for (size_t i = 0; i < _va_count; ++i) {\n";
        out << "    _t_" << vaname << "->settable(clx::LValue(static_cast<double>(i + 1)), _va_args[i]);\n";
        out << "}\n";
        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << vaname << ".val, &l_" << vaname
            << ".type);\n";
    }

    size_t prev_locals = locals.size();
    for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
        uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
        bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
        bool param_is_boxed = is_cap && !state.constant_upvalues.count(pname);
        locals.push_back({ pname, param_is_boxed });
    }
    if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
        std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name,
            ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
        locals.push_back({ vaname, false });
    }

    if (node.as.func_def.body_block != 0xFFFFFFFF) {
        emit_node(node.as.func_def.body_block);
    }

    locals.resize(prev_locals);
    state.native_numbers.resize(prev_native_count_for_params);
    state.in_function_def = prev_in_func;
    state.current_func_body = saved_func_body;
    state.direct_callables = std::move(saved_direct_callables);
    state.fast_callables = std::move(saved_fast_callables);
    if (state.arena_table_sizes.count(node_idx))
        out << "clx::arena_reset(&_arena);\n";
    state.current_arena_func = saved_arena_func;
    out << "return clx::MultiValue();\n";
    out << "}";
    if (!is_raw)
        out << ", static_cast<clx::LTable*>(_ENV.as_pointer()))";
}

//------------------ emitReturnStatement: handles NodeType::ReturnStatement
void CodeEmitter::emitReturnStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    uint32_t v_count = node.as.return_stmt.value_count;
    uint32_t first_v = node.as.return_stmt.first_value;

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

    if (v_count == 0) {
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
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

        out << "{\n";
        bool last_expands = false;
        uint32_t last_arg = 0xFFFFFFFF;
        if (call_node.as.call_expr.arg_count > 0) {
            last_arg = ctx.block_statements[call_node.as.call_expr.first_arg + call_node.as.call_expr.arg_count - 1];
            if (ctx.nodes[last_arg].type == NodeType::CallExpression || ctx.nodes[last_arg].type == NodeType::Vararg)
                last_expands = true;
        }

        if (last_expands) {
            out << "    clx::LValue _dyn_buf[16];\n    size_t _dyn_count = 0;\n";
            for (uint32_t a = 0; a < call_node.as.call_expr.arg_count - 1; ++a) {
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

            if (!is_direct)
                out << "    for (size_t i = 0; i < _dyn_count; ++i) L->shadow_stack[L->shadow_top++] = "
                       "clx::TypedSlot(&_dyn_buf[i].val, &_dyn_buf[i].type);\n";
            if (is_direct) {
                if (state.in_function_def) {
                    if (state.current_arena_func != 0xFFFFFFFF)
                        out << "    clx::arena_reset(&_arena);\n";
                    out << "    CLX_MUSTTAIL return _impl_" << fname << "(L, _dyn_buf, _dyn_count);\n";
                } else {
                    out << "    clx::MultiValue _res = _impl_" << fname << "(L, _dyn_buf, _dyn_count);\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else {
                if (state.in_function_def) {
                    if (state.current_arena_func != 0xFFFFFFFF)
                        out << "    clx::arena_reset(&_arena);\n";
                    out << "    CLX_MUSTTAIL return clx::call_function(L, ";
                    emit_node(tgt);
                    out << ", _dyn_buf, _dyn_count, \"" << ctx.filename << "\", " << call_node.line << ");\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_function(L, ";
                    emit_node(tgt);
                    out << ", _dyn_buf, _dyn_count, \"" << ctx.filename << "\", " << call_node.line << ");\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            }
        } else {
            if (call_node.as.call_expr.arg_count > 0) {
                out << "    clx::LValue args_" << last_v_idx << "[] = {";
                for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                    emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                    if (a < call_node.as.call_expr.arg_count - 1)
                        out << ", ";
                }
                out << "};\n";
                if (!is_direct)
                    out << "    for (size_t i = 0; i < " << call_node.as.call_expr.arg_count
                        << "; ++i) L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args_" << last_v_idx
                        << "[i].val, &args_" << last_v_idx << "[i].type);\n";

                if (is_direct) {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    CLX_MUSTTAIL return _impl_" << fname << "(L, args_" << last_v_idx << ", "
                            << call_node.as.call_expr.arg_count << ");\n";
                    } else {
                        out << "    clx::MultiValue _res = _impl_" << fname << "(L, args_" << last_v_idx << ", "
                            << call_node.as.call_expr.arg_count << ");\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    CLX_MUSTTAIL return clx::call_function(L, ";
                        emit_node(tgt);
                        out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", \""
                            << ctx.filename << "\", " << call_node.line << ");\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_function(L, ";
                        emit_node(tgt);
                        out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", \""
                            << ctx.filename << "\", " << call_node.line << ");\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                }
            } else {
                if (is_direct) {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    CLX_MUSTTAIL return _impl_" << fname << "(L, nullptr, 0);\n";
                    } else {
                        out << "    clx::MultiValue _res = _impl_" << fname << "(L, nullptr, 0);\n";
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
        out << "    clx::LValue _ret_args[" << std::max(1u, v_count) << "];\n";
        for (size_t i = 0; i < v_count; ++i) {
            out << "    _ret_args[" << i << "] = ";
            emit_node(ctx.block_statements[first_v + i]);
            out << ";\n";
        }
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "    clx::arena_reset(&_arena);\n";
            out << "    return clx::MultiValue(_ret_args, " << v_count << ");\n";
        } else {
            out << "    return _ret_args[0];\n";
        }
    } else {
        out << "    std::vector<clx::LValue> _ret_vals;\n";
        for (size_t i = 0; i < v_count; ++i) {
            uint32_t v_idx = ctx.block_statements[first_v + i];
            if (i == v_count - 1 && (last_is_call || ctx.nodes[v_idx].type == NodeType::Vararg)) {
                state.expect_multivalue = true;
                out << "    clx::MultiValue _mret = ";
                emit_node(v_idx);
                out << ";\n";
                state.expect_multivalue = false;
                out << "    for (size_t m = 0; m < _mret.count; ++m) _ret_vals.push_back(_mret[m]);\n";
            } else {
                out << "    clx::LValue _tmp_" << node_idx << "_" << i << " = ";
                emit_node(v_idx);
                out << ";\n";
                out << "    _ret_vals.push_back(_tmp_" << node_idx << "_" << i << ");\n";
            }
        }
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "    clx::arena_reset(&_arena);\n";
            out << "    return clx::MultiValue(_ret_vals);\n";
        } else {
            out << "    return (!_ret_vals.empty()) ? _ret_vals[0] : clx::LValue();\n";
        }
    }
    out << "}\n";
}

//------------------ emitAssignmentLike: handles NodeType::GlobalDeclStatement, NodeType::LocalDecl, NodeType::Assignment
void CodeEmitter::emitAssignmentLike(const ASTNode &node, uint32_t node_idx) {
    if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard)
        return;
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";

    bool is_local = (node.type == NodeType::LocalDecl);
    bool is_global = (node.type == NodeType::GlobalDeclStatement);
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
            bool rhs_yields_number
                = yields_number(ctx, state, ctx.block_statements[first_v], nullptr, state.current_fast_func);
            if (!ctx.nodes[t_idx].as.ident.is_captured) {
                if (is_local) {
                    if (in_native || rhs_yields_number)
                        is_single_native = true;
                } else {
                    if (in_native && rhs_yields_number)
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
            if (!state.hoisted_locals.count(single_name)) {
                bool is_int_single = state.native_integers.count(single_name) > 0;
                if (!is_int_single && v_count > 0) {
                    uint32_t v_idx_single = ctx.block_statements[first_v];
                    if (clx::is_purely_integer_expr(ctx, state, v_idx_single)
                        && !var_reassigned_non_int(single_name, state.current_func_body)) {
                        is_int_single = true;
                        state.native_integers.insert(std::string(single_name));
                    }
                }
                if (is_int_single)
                    out << "int64_t l_" << single_name << ";\n";
                else
                    out << "double l_" << single_name << ";\n";
            }
            out << "l_" << single_name << " = ";
            if (v_count > 0) {
                uint32_t v_idx = ctx.block_statements[first_v];
                emit_native(v_idx);
            } else
                out << "0.0";
            out << ";\n";
            locals.push_back({ single_name, false });
        } else {
            bool is_boxed = false;
            bool is_loc = this->is_local(single_name, is_boxed);
            if (is_boxed) {
                out << "(*l_" << single_name << ") = clx::LValue(static_cast<double>(";
                if (v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    emit_native(v_idx);
                } else
                    out << "0.0";
                out << "))";
            } else {
                out << "l_" << single_name << " = ";
                if (v_count > 0) {
                    uint32_t v_idx = ctx.block_statements[first_v];
                    emit_native(v_idx);
                } else
                    out << "0.0";
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

                                out << "auto _fast_" << name << " = [&]( ";
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
                                out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << name
                                    << "(";
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                    if (a < state.func_param_counts[name] - 1)
                                        out << ", ";
                                }
                                out << "))));\n};\n";
                                out << "clx::LValue l_" << name << " = L->create_closure(_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer()));\n";
                                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_"
                                    << name << ".type);\n";
                                {
                                    size_t _sfi = std::distance(state.string_pool.begin(),
                                        std::find(state.string_pool.begin(), state.string_pool.end(), name));
                                    out << "L->_G->settable(cstr_[" << _sfi << "], l_" << name << ");\n";
                                }
                                locals.push_back({ name, false });
                                state.fast_callables.insert(name);
                            } else {
                                out << "clx::LValue l_" << name << ";\n";
                                state.ref_capture
                                    = std::string("l_") + std::string(name) + ", &_impl_" + std::string(name);
                                if (state.string_builders.count(name) && !state.module_string_builders.count(name)) {
                                    state.ref_capture += ", sb_" + std::string(name);
                                }
                                locals.push_back({ name, false });
                                state.direct_callables.insert(name);
                                out << "std::function<clx::MultiValue(clx::LState*, const clx::LValue*, size_t)> _impl_"
                                    << name << ";\n";
                                out << "_impl_" << name << " = ";
                                state.emit_raw_lambda = true;
                                emit_node(v_idx);
                                state.emit_raw_lambda = false;
                                state.ref_capture.clear();
                                out << ";\n";
                                out << "l_" << name << " = L->create_closure(_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer()));\n";
                                out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_"
                                    << name << ".type);\n";
                                {
                                    size_t _sfi = std::distance(state.string_pool.begin(),
                                        std::find(state.string_pool.begin(), state.string_pool.end(), name));
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
                        if (state.table_presize.count(v_idx)) {
                            out << "std::vector<double> l_" << name << "(static_cast<size_t>(";
                            emit_native(state.table_presize[v_idx]);
                            out << "), 0.0);\n";
                        } else {
                            out << "std::vector<double> l_" << name << ";\n";
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
                            if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                                && !state.module_string_builders.count(name)) {
                                out << "clx::StringBuilder sb_" << name << ";\n";
                            }
                            locals.push_back({ name, false });

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
                                state.ref_capture = std::string("l_") + std::string(name);
                                if (state.string_builders.count(name) && !state.module_string_builders.count(name)) {
                                    state.ref_capture += ", sb_" + std::string(name);
                                }
                            }
                            locals.push_back({ name, true });
                            out << "l_" << name << " = clx::make_upvalue(";
                            if (v_count > 0)
                                emit_node(ctx.block_statements[first_v]);
                            else
                                out << "clx::LValue()";
                            out << ");\nL->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << "->val, &l_"
                                << name << "->type);\n";
                            if (is_func) {
                                state.ref_capture.clear();
                                size_t _sfi = std::distance(state.string_pool.begin(),
                                    std::find(state.string_pool.begin(), state.string_pool.end(), name));
                                out << "L->_G->settable(cstr_[" << _sfi << "], *l_" << name << ");\n";
                            }
                            if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                                && !state.module_string_builders.count(name)) {
                                out << "clx::StringBuilder sb_" << name << ";\n";
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
                        if (state.string_builders.count(name) && !state.global_string_builders.count(name)
                            && !state.module_string_builders.count(name)) {
                            out << "clx::StringBuilder sb_" << name << ";\n";
                        }
                        locals.push_back({ name, false });

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
                        out << "  if (sb_" << name << ".empty() && _gval.type == clx::ValueType::String) {\n";
                        out << "    sb_" << name << ".append(_gval.as_string(), _gval.string_len()); }\n";
                        for (size_t i = 1; i < ops.size(); ++i) {
                            uint32_t op_idx = ops[i];
                            const auto &op_node = ctx.nodes[op_idx];
                            if (op_node.type == NodeType::String) {
                                size_t sfi = std::distance(state.string_pool.begin(),
                                    std::find(state.string_pool.begin(), state.string_pool.end(),
                                        std::string_view(op_node.as.string.text, op_node.as.string.length)));
                                out << "  sb_" << name << ".append(cstr_[" << sfi << "].as_string(), cstr_[" << sfi
                                    << "].string_len());\n";
                            } else if (op_node.type == NodeType::Integer) {
                                out << "  sb_" << name << ".append(L, clx::LValue(static_cast<double>("
                                    << op_node.as.integer.val << ".0)));\n";
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

                                out << "auto _fast_" << name << " = [&]( ";
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
                                out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << name
                                    << "(";
                                for (uint32_t a = 0; a < state.func_param_counts[name]; ++a) {
                                    out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                    if (a < state.func_param_counts[name] - 1)
                                        out << ", ";
                                }
                                out << "))));\n};\n";
                                state.fast_callables.insert(name);
                            } else {
                                out << "auto _impl_" << name << " = ";
                                state.emit_raw_lambda = true;
                                emit_node(v_idx);
                                state.emit_raw_lambda = false;
                                out << ";\n";
                            }

                            state.direct_callables.insert(name);
                            if (is_boxed)
                                out << "(*l_" << name << ") = L->create_closure(_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer()));\n";
                            else
                                out << "l_" << name << " = L->create_closure(_impl_" << name
                                    << ", static_cast<clx::LTable*>(_ENV.as_pointer()));\n";
                            {
                                size_t _sfi2 = std::distance(state.string_pool.begin(),
                                    std::find(state.string_pool.begin(), state.string_pool.end(), name));
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
                            if (first_name == name && !ctx.nodes[concat_ops[0]].as.ident.is_global) {

                                if (is_boxed) {
                                    out << "if (sb_" << name << ".empty()) sb_" << name << ".append((*l_" << name
                                        << ").as_string(), (*l_" << name << ").string_len());\n";
                                } else {
                                    out << "if (sb_" << name << ".empty()) sb_" << name << ".append(l_" << name
                                        << ".as_string(), l_" << name << ".string_len());\n";
                                }

                                for (size_t i = 1; i < concat_ops.size(); ++i) {
                                    uint32_t op_idx = concat_ops[i];
                                    const auto &op_node = ctx.nodes[op_idx];
                                    if (op_node.type == NodeType::String) {
                                        size_t sfi = std::distance(state.string_pool.begin(),
                                            std::find(state.string_pool.begin(), state.string_pool.end(),
                                                std::string_view(op_node.as.string.text, op_node.as.string.length)));
                                        out << "  sb_" << name << ".append(cstr_[" << sfi << "].as_string(), cstr_["
                                            << sfi << "].string_len());\n";
                                    } else if (op_node.type == NodeType::Integer) {
                                        out << "sb_" << name << ".append(L, clx::LValue(static_cast<double>("
                                            << op_node.as.integer.val << ".0)));\n";
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
                        out << "(*l_" << name << ") = ";
                    else
                        out << "l_" << name << " = ";

                    if (v_count > 0) {
                        bool lhs_is_native = !is_boxed
                            && std::find(state.native_numbers.begin(), state.native_numbers.end(), name)
                                != state.native_numbers.end();
                        if (lhs_is_native)
                            emit_native(ctx.block_statements[first_v]);
                        else
                            emit_node(ctx.block_statements[first_v]);
                    } else
                        out << "clx::LValue()";
                    out << ";\n";
                }
            } else {
                size_t idx = std::distance(
                    state.string_pool.begin(), std::find(state.string_pool.begin(), state.string_pool.end(), name));
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
                                    emitTableOp(bin_op, lhs_tbl, lhs_key, const_idx);
                                    return;
                                }
                            }
                        }
                    }
                }
            }

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
                out << "{ auto* _t" << t_idx << " = static_cast<clx::LTable*>((";
                emit_node(t_node.as.table_access.table);
                out << ").as_pointer()); size_t _k" << t_idx << " = static_cast<size_t>(";
                emit_native(t_node.as.table_access.key);
                out << ") - 1; auto _sv" << t_idx << " = ";
                if (v_count > 0)
                    emit_node(ctx.block_statements[first_v]);
                else
                    out << "clx::LValue()";
                out << "; _t" << t_idx << "->array[_k" << t_idx << "] = _sv" << t_idx << ".val; _t" << t_idx
                    << "->array_types[_k" << t_idx << "] = _sv" << t_idx << ".type; }\n";
            } else {
                bool key_is_native = false;
                uint32_t k_idx = t_node.as.table_access.key;
                if (yields_number(ctx, state, k_idx, nullptr, state.current_fast_func))
                    key_is_native = true;

                if (key_is_native) {
                    out << "{ clx::LTable* _t = static_cast<clx::LTable*>((";
                    emit_node(t_node.as.table_access.table);
                    out << ").as_pointer()); size_t _k = static_cast<size_t>(";
                    emit_native(k_idx);
                    out << "); if (_k - 1 < _t->array_size) { clx::LValue _sv = ";
                    if (v_count > 0)
                        emit_node(ctx.block_statements[first_v]);
                    else
                        out << "clx::LValue()";
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
                            auto _it = state.numeric_table_fields.find(_tn);
                            if (_it != state.numeric_table_fields.end() && _it->second.count(_kn))
                                is_known_field = true;
                        }
                        int _cs_i = -1;
                        size_t _cstr_idx = std::distance(state.string_pool.begin(),
                            std::find(state.string_pool.begin(), state.string_pool.end(),
                                std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length)));
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
                                                emitTableOp(bin_op, lhs_tbl, lhs_key, const_idx);
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

                            out << "auto _fast_" << fname << " = [&]( ";
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
                            out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << fname << "(";
                            for (uint32_t a = 0; a < state.func_param_counts[fname]; ++a) {
                                out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                if (a < state.func_param_counts[fname] - 1)
                                    out << ", ";
                            }
                            out << "))));\n};\n";
                            state.fast_callables.insert(fname);
                        } else {
                            out << "auto _impl_" << fname << " = ";
                            state.emit_raw_lambda = true;
                            emit_node(v_idx);
                            state.emit_raw_lambda = false;
                            out << ";\n";
                        }

                        out << "clx::LValue _tmp_" << node_idx << "_" << i << " = L->create_closure(_impl_" << fname
                            << ", static_cast<clx::LTable*>(_ENV.as_pointer()));\n";
                        state.direct_callables.insert(fname);
                        intercepted = true;
                    }
                }
            }

            if (intercepted)
                continue;

            if (i == v_count - 1 && last_is_call) {
                state.expect_multivalue = true;
                out << "_mret_" << node_idx << " = ";
                emit_node(v_idx);
                out << ";\n";
                state.expect_multivalue = false;
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
                            && !var_reassigned_non_int(tname, state.current_func_body);
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
                }
            }
        }
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

            if (is_cap) {
                if (state.constant_upvalues.count(name)) {
                    if (!state.hoisted_locals.count(name)) {
                        out << "clx::LValue l_" << name << ";\n";
                        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << name << ".val, &l_" << name
                            << ".type);\n";
                    }
                    locals.push_back({ name, false });
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
                bool is_int_var = state.native_integers.count(name) > 0;
                if (!is_int_var && v_count > 0) {
                    uint32_t v_idx_check
                        = (first_v + i < ctx.block_statements.size()) ? ctx.block_statements[first_v + i] : 0xFFFFFFFF;
                    if (clx::is_purely_integer_expr(ctx, state, v_idx_check)
                        && !var_reassigned_non_int(name, state.current_func_body)) {
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
                        out << "l_" << name << " = 0.0;\n";
                    }
                }
                locals.push_back({ name, false });
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
            }
        }
    }

    for (size_t i = 0; i < t_count; ++i) {
        std::string val_str;
        std::string num_str;

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
                    out << "(*l_" << name << ") = " << val_str << ";\n";
                else if (!is_local && is_boxed)
                    out << "(*l_" << name << ") = " << val_str << ";\n";
                else if (is_n)
                    out << "l_" << name << " = " << num_str << ";\n";
                else
                    out << "l_" << name << " = " << val_str << ";\n";
            } else if (is_global && v_count == 0) {
            } else if (t_node.as.ident.is_global) {
                out << "clx::set_env_var(L, _ENV, \"" << name << "\", " << val_str << ");\n";
            } else {
                size_t idx = std::distance(
                    state.string_pool.begin(), std::find(state.string_pool.begin(), state.string_pool.end(), name));
                out << "L->_G->settable(cstr_[" << idx << "], " << val_str << ");\n";
            }
        } else if (t_node.type == NodeType::TableAccess) {
            std::string_view t_name;
            if (ctx.nodes[t_node.as.table_access.table].type == NodeType::Identifier) {
                t_name = std::string_view(ctx.nodes[t_node.as.table_access.table].as.ident.name,
                    ctx.nodes[t_node.as.table_access.table].as.ident.length);
            }

            if (!t_name.empty() && state.pure_numeric_arrays.count(t_name)) {
                out << "l_" << t_name << "[static_cast<size_t>(";
                emit_native(t_node.as.table_access.key);
                out << ") - 1] = " << num_str << ";\n";
            } else if (state.bce_safe_nodes.count(t_idx)) {
                out << "{ auto* _t" << t_idx << " = static_cast<clx::LTable*>((";
                emit_node(t_node.as.table_access.table);
                out << ").as_pointer()); size_t _k" << t_idx << " = static_cast<size_t>(";
                emit_native(t_node.as.table_access.key);
                out << ") - 1; _t" << t_idx << "->array[_k" << t_idx << "] = " << val_str << ".val; _t" << t_idx
                    << "->array_types[_k" << t_idx << "] = " << val_str << ".type; }\n";
            } else {
                bool key_is_native = false;
                uint32_t k_idx = t_node.as.table_access.key;
                if (yields_number(ctx, state, k_idx, nullptr, state.current_fast_func))
                    key_is_native = true;

                if (key_is_native) {
                    out << "{ clx::LTable* _t = static_cast<clx::LTable*>((";
                    emit_node(t_node.as.table_access.table);
                    out << ").as_pointer()); size_t _k = static_cast<size_t>(";
                    emit_native(k_idx);
                    out << "); if (_k - 1 < _t->array_size) [[likely]] { _t->array[_k - 1] = " << val_str
                        << ".val; _t->array_types[_k - 1] = " << val_str
                        << ".type; } else clx::table_set_int(L, clx::LValue(clx::ValueType::Table, _t), _k, " << val_str
                        << "); }\n";
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
                            auto _it = state.numeric_table_fields.find(_tn);
                            if (_it != state.numeric_table_fields.end() && _it->second.count(_kn))
                                is_known_field = true;
                        }
                        size_t _cstr_idx = std::distance(state.string_pool.begin(),
                            std::find(state.string_pool.begin(), state.string_pool.end(),
                                std::string_view(ctx.nodes[kt2].as.string.text, ctx.nodes[kt2].as.string.length)));
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
}

//------------------ emitDoStatement: handles NodeType::DoStatement
void CodeEmitter::emitDoStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    size_t prev_locals = locals.size();
    if (node.as.do_stmt.body_block != 0xFFFFFFFF)
        emit_node(node.as.do_stmt.body_block);
    locals.resize(prev_locals);
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
                    out << "clx::LValue(static_cast<double>(-(";
                    emit_native(node.as.unary_op.expr);
                    out << ")))";
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

//------------------ emitBinaryOp: handles NodeType::BinaryOp
void CodeEmitter::emitBinaryOp(const ASTNode &node, uint32_t node_idx) {
    int op = node.as.bin_op.op;

    bool left_native = yields_number(ctx, state, node.as.bin_op.left, nullptr, state.current_fast_func);
    bool right_native = yields_number(ctx, state, node.as.bin_op.right, nullptr, state.current_fast_func);

    if (left_native && right_native) {
        bool both_int = clx::is_purely_integer_expr(ctx, state, node.as.bin_op.left)
            && clx::is_purely_integer_expr(ctx, state, node.as.bin_op.right);
        if (op == static_cast<int>(BinaryOp::Add)) {
            out << (both_int ? "clx::LValue(static_cast<int64_t>(" : "clx::LValue(static_cast<double>(");
            emit_native(node.as.bin_op.left);
            out << " + ";
            emit_native(node.as.bin_op.right);
            out << "))";
            return;
        }
        if (op == static_cast<int>(BinaryOp::Sub)) {
            out << (both_int ? "clx::LValue(static_cast<int64_t>(" : "clx::LValue(static_cast<double>(");
            emit_native(node.as.bin_op.left);
            out << " - ";
            emit_native(node.as.bin_op.right);
            out << "))";
            return;
        }
        if (op == static_cast<int>(BinaryOp::Mul)) {
            out << (both_int ? "clx::LValue(static_cast<int64_t>(" : "clx::LValue(static_cast<double>(");
            emit_native(node.as.bin_op.left);
            out << " * ";
            emit_native(node.as.bin_op.right);
            out << "))";
            return;
        }
        if (op == static_cast<int>(BinaryOp::Div)) {
            out << "clx::LValue(static_cast<double>(";
            emit_native(node.as.bin_op.left);
            out << ") / static_cast<double>(";
            emit_native(node.as.bin_op.right);
            out << "))";
            return;
        }
        if (op == static_cast<int>(BinaryOp::Mod)) {
            if (both_int) {
                out << "clx::LValue(static_cast<int64_t>(static_cast<int64_t>(";
                emit_native(node.as.bin_op.left);
                out << ") % static_cast<int64_t>(";
                emit_native(node.as.bin_op.right);
                out << ")))";
                return;
            }
            out << "clx::LValue(static_cast<double>(std::fmod(";
            emit_native(node.as.bin_op.left);
            out << ", ";
            emit_native(node.as.bin_op.right);
            out << ")))";
            return;
        }
        if (op == static_cast<int>(BinaryOp::FloorDiv)) {
            out << "clx::LValue(static_cast<double>(std::floor((";
            emit_native(node.as.bin_op.left);
            out << ") / (";
            emit_native(node.as.bin_op.right);
            out << "))))";
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
                    std::string d = lua_decode_string(s);
                    out << "clx_memcpy(_p, \"" << cpp_escape(d) << "\", " << d.length() << "); _p += " << d.length()
                        << "; ";
                } else {
                    out << "_p += std::snprintf(_p, " << (total_str_len + 1)
                        << " - (_p - _buf), \"%g\", static_cast<double>(";
                    emit_native(op);
                    out << ")); ";
                }
            }
            out << "*_p = '\\0'; return clx::LValue(L->intern_string(_buf, static_cast<size_t>(_p - _buf))); }())";
        } else {
            out << "([&](){\n";
            out << "    clx::LValue args[] = {";
            for (size_t i = 0; i < operands.size(); ++i) {
                emit_node(operands[i]);
                if (i < operands.size() - 1)
                    out << ", ";
            }
            out << "};\n";
            out << "    return clx::concat_multi(L, args, " << operands.size() << ");\n";
            out << "}())";
        }
        return;
    }
    if (op == static_cast<int>(BinaryOp::And)) {
        out << "([&](){ clx::LValue _a = ";
        emit_node(node.as.bin_op.left);
        out << "; return _a.as_bool() ? (";
        emit_node(node.as.bin_op.right);
        out << ") : _a; }())";
        return;
    }
    if (op == static_cast<int>(BinaryOp::Or)) {
        out << "([&](){ clx::LValue _a = ";
        emit_node(node.as.bin_op.left);
        out << "; return _a.as_bool() ? _a : (";
        emit_node(node.as.bin_op.right);
        out << "); }())";
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
        if (op == static_cast<int>(BinaryOp::Ne))
            out << "clx::LValue(!(";
        out << "clx::" << fn[op] << "(L, ";
        if (op == static_cast<int>(BinaryOp::Gt) || op == static_cast<int>(BinaryOp::Ge)) {
            emit_node(node.as.bin_op.right);
            out << ", ";
            emit_node(node.as.bin_op.left);
        } else {
            emit_node(node.as.bin_op.left);
            out << ", ";
            emit_node(node.as.bin_op.right);
        }
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

void CodeEmitter::emitTableOp(int bin_op, uint32_t lhs_tbl, uint32_t lhs_key, uint32_t const_idx) {
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
    out << ");\n";
}

//------------------ emitNumber: handles NodeType::Number
void CodeEmitter::emitNumber(const ASTNode &node, uint32_t node_idx) {
    out << "clx::LValue(static_cast<double>(" << node.as.number.val << "))";
}

//------------------ emitInteger: handles NodeType::Integer
void CodeEmitter::emitInteger(const ASTNode &node, uint32_t node_idx) {
    out << "clx::integer(static_cast<int64_t>(" << node.as.integer.val << "))";
}

//------------------ emitIdentifier: handles NodeType::Identifier
void CodeEmitter::emitIdentifier(const ASTNode &node, uint32_t node_idx) {
    std::string_view name(node.as.ident.name, node.as.ident.length);
    bool is_native
        = std::find(state.native_numbers.begin(), state.native_numbers.end(), name) != state.native_numbers.end();
    bool is_boxed = false;
    bool is_loc = this->is_local(name, is_boxed);

    if (node.as.ident.is_global) {
        if (name == "_ENV") {
            out << "_ENV";
        } else if (state.global_constants.count(name)) {
            out << "clx::LValue(static_cast<double>(" << state.global_constants[name] << "))";
        } else if (state.string_builders.count(name)) {
            out << "(sb_" << name << ".empty() ? clx::get_env_var(L, _ENV, \"" << name << "\") : clx::LValue(sb_"
                << name << ".to_string(L)))";
        } else {
            out << "clx::get_env_var(L, _ENV, \"" << name << "\")";
        }
    } else if (is_native && !is_boxed && !node.as.ident.is_captured) {
        out << "clx::LValue(l_" << name << ")";
    } else if (is_loc) {
        if (state.string_builders.count(name)) {
            if (is_boxed) {
                out << "(sb_" << name << ".empty() ? (*l_" << name << ") : clx::LValue(sb_" << name
                    << ".to_string(L)))";
            } else {
                out << "(sb_" << name << ".empty() ? l_" << name << " : clx::LValue(sb_" << name << ".to_string(L)))";
            }
        } else if (is_boxed)
            out << "(*l_" << name << ")";
        else if (is_native)
            out << "clx::LValue(l_" << name << ")";
        else
            out << "l_" << name;
    } else {
        size_t idx = std::distance(
            state.string_pool.begin(), std::find(state.string_pool.begin(), state.string_pool.end(), name));
        out << "clx_gettable_safe(L->_G->gettable(cstr_[" << idx << "]))";
    }
}

//------------------ emitString: handles NodeType::String
void CodeEmitter::emitString(const ASTNode &node, uint32_t node_idx) {
    std::string_view s(node.as.string.text, node.as.string.length);
    size_t idx
        = std::distance(state.string_pool.begin(), std::find(state.string_pool.begin(), state.string_pool.end(), s));
    out << "cstr_[" << idx << "]";
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
    out << "while (";
    emit_condition(node.as.while_stmt.condition);
    out << ")\n";
    if (node.as.while_stmt.body_block != 0xFFFFFFFF)
        emit_node(node.as.while_stmt.body_block);
}

//------------------ emitRepeatStatement: handles NodeType::RepeatStatement
void CodeEmitter::emitRepeatStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    out << "do\n";
    if (node.as.repeat_stmt.body_block != 0xFFFFFFFF)
        emit_node(node.as.repeat_stmt.body_block);
    out << "while (!(";
    emit_condition(node.as.repeat_stmt.condition);
    out << "));\n";
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
    int64_t step_int_val = 1;
    if (!step_is_default) {
        auto &step_node = ctx.nodes[node.as.for_stmt.step_expr];
        if (step_node.type == NodeType::Number) {
            double step_val = step_node.as.number.val;
            step_known_positive = (step_val > 0);
            step_known_negative = (step_val < 0);
            step_int_val = static_cast<int64_t>(step_val);
        } else if (step_node.type == NodeType::Integer) {
            int64_t step_val = step_node.as.integer.val;
            step_known_positive = (step_val > 0);
            step_known_negative = (step_val < 0);
            step_int_val = step_val;
        }
    }

    bool start_is_int_literal = ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::Integer;
    if (!start_is_int_literal && ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::BinaryOp) {
        auto &bin = ctx.nodes[node.as.for_stmt.start_expr].as.bin_op;
        if (bin.op == static_cast<int>(BinaryOp::Add) || bin.op == static_cast<int>(BinaryOp::Sub)) {
            start_is_int_literal = (ctx.nodes[bin.left].type == NodeType::Integer
                                       && yields_number(ctx, state, bin.right, nullptr, state.current_fast_func))
                || (ctx.nodes[bin.right].type == NodeType::Integer
                    && yields_number(ctx, state, bin.left, nullptr, state.current_fast_func));
        }
    }
    bool counter_is_int = native_for && !is_cap && start_is_int_literal
        && (step_is_default || (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Integer && step_int_val == 1));

    out << "{\n";
    if (!native_for)
        out << "clx::ScopeGuard _sg_for_" << node_idx << "(L);\n";

    if (native_for) {
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
            out << "int64_t l_" << node_idx << " = ";
            emit_native(node.as.for_stmt.limit_expr);
            out << ";\n";
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
        }
    } else {
        bool start_is_literal = (ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::Integer
            || ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::Number);
        bool step_can_skip = step_is_default || (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Integer)
            || (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Number);
        if (start_is_literal && step_can_skip) {
            out << "double s_" << node_idx << " = ";
            if (ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::Integer)
                out << "static_cast<double>(" << ctx.nodes[node.as.for_stmt.start_expr].as.integer.val << ")";
            else
                out << ctx.nodes[node.as.for_stmt.start_expr].as.number.val;
            out << ";\n";
            out << "clx::LValue limit_" << node_idx << " = ";
            emit_node(node.as.for_stmt.limit_expr);
            out << ";\n";
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&limit_" << node_idx << ".val, &limit_"
                << node_idx << ".type);\n";
            if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
                out << "double st_" << node_idx << " = ";
                if (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Integer)
                    out << "static_cast<double>(" << ctx.nodes[node.as.for_stmt.step_expr].as.integer.val << ")";
                else
                    out << ctx.nodes[node.as.for_stmt.step_expr].as.number.val;
                out << ";\n";
            } else {
                out << "double st_" << node_idx << " = 1.0;\n";
            }
            out << "double l_" << node_idx << ";\n";
            out << "if (!limit_" << node_idx << ".to_number(l_" << node_idx << ")) {\n";
            out << "std::cerr << \"Error: " << ctx.filename << ":" << node.line
                << ": 'for' initial values must be numeric\\n\"; std::exit(1);\n}\n";
        } else {
            out << "clx::LValue start_" << node_idx << " = ";
            emit_node(node.as.for_stmt.start_expr);
            out << ";\n";
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&start_" << node_idx << ".val, &start_"
                << node_idx << ".type);\n";
            out << "clx::LValue limit_" << node_idx << " = ";
            emit_node(node.as.for_stmt.limit_expr);
            out << ";\n";
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&limit_" << node_idx << ".val, &limit_"
                << node_idx << ".type);\n";

            if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
                out << "clx::LValue step_" << node_idx << " = ";
                emit_node(node.as.for_stmt.step_expr);
                out << ";\n";
            } else {
                out << "clx::LValue step_" << node_idx << " = clx::LValue(static_cast<double>(1.0));\n";
            }
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&step_" << node_idx << ".val, &step_" << node_idx
                << ".type);\n";

            out << "double s_" << node_idx << ", l_" << node_idx << ", st_" << node_idx << ";\n";
            out << "if (!start_" << node_idx << ".to_number(s_" << node_idx << ") || !limit_" << node_idx
                << ".to_number(l_" << node_idx << ") || !step_" << node_idx << ".to_number(st_" << node_idx << ")) {\n";
            out << "std::cerr << \"Error: " << ctx.filename << ":" << node.line
                << ": 'for' initial values must be numeric\\n\"; std::exit(1);\n}\n";
        }
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
            if (tbl < ctx.nodes.size() && key < ctx.nodes.size() && ctx.nodes[tbl].type == NodeType::Identifier
                && ctx.nodes[tbl].as.ident.is_global && ctx.nodes[key].type == NodeType::String) {
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
        out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&" << _h_name << ".val, &" << _h_name << ".type);\n";
        state.hoisted_lookups[_h_node] = _h_name;
        uint32_t _ht = ctx.nodes[_h_node].as.table_access.table;
        uint32_t _hk = ctx.nodes[_h_node].as.table_access.key;
        if (_ht < ctx.nodes.size() && _hk < ctx.nodes.size() && ctx.nodes[_ht].type == NodeType::Identifier
            && ctx.nodes[_hk].type == NodeType::String) {
            std::string_view _hm(ctx.nodes[_ht].as.ident.name, ctx.nodes[_ht].as.ident.length);
            std::string_view _hf(ctx.nodes[_hk].as.string.text, ctx.nodes[_hk].as.string.length);
            const char *_cf = lookup_builtin(_hm, _hf);
            if (_cf)
                state.hoisted_cfuncs[_h_name] = _cf;
        }
    }

    auto emit_for_body = [&]() {
        if (is_cap || !is_n) {
            out << "clx::ScopeGuard _sg_iter(L);\n";
        }

        if (is_cap) {
            out << "clx::LUpValue l_" << var_name << ";\n";
            out << "l_" << var_name << " = clx::make_upvalue(clx::LValue(i_val));\n";
            out << "L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << var_name << "->val, &l_" << var_name
                << "->type);\n";
        } else if (is_n) {
            if (counter_is_int) {
                out << "const int64_t l_" << var_name << " = i_val;\n";
                state.native_integers.insert(std::string(var_name));
                state.native_numbers.push_back(var_name);
            } else {
                out << "const double l_" << var_name << " = i_val;\n";
                state.native_numbers.push_back(var_name);
            }
        } else {
            out << "const clx::LValue l_" << var_name
                << "( (i_val == static_cast<int64_t>(i_val)) ? clx::LValue(static_cast<int64_t>(i_val)) : "
                   "clx::LValue(i_val) );\n";
        }

        size_t prev_locals = locals.size();
        locals.push_back({ var_name, is_cap });
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
    };

    if (counter_is_int) {
        auto &start_node = ctx.nodes[node.as.for_stmt.start_expr];
        bool start_needs_runtime = (start_node.type == NodeType::BinaryOp);
        if (step_known_positive) {
            if (start_needs_runtime) {
                out << "for (int64_t i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val++) {\n";
            } else {
                int64_t start_int = start_node.as.integer.val;
                out << "for (int64_t i_val = " << start_int << "; i_val <= l_" << node_idx << "; i_val++) {\n";
            }
            emit_for_body();
            out << "}\n";
        } else {
            if (start_needs_runtime) {
                out << "for (int64_t i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val--) {\n";
            } else {
                int64_t start_int = start_node.as.integer.val;
                out << "for (int64_t i_val = " << start_int << "; i_val >= l_" << node_idx << "; i_val--) {\n";
            }
            emit_for_body();
            out << "}\n";
        }
    } else if (step_known_positive) {
        out << "for (double i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_" << node_idx
            << ") {\n";
        emit_for_body();
        out << "}\n";
    } else if (step_known_negative) {
        out << "for (double i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_" << node_idx
            << ") {\n";
        emit_for_body();
        out << "}\n";
    } else {
        out << "if (st_" << node_idx << " > 0) {\n";
        out << "    for (double i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_" << node_idx
            << ") {\n";
        emit_for_body();
        out << "    }\n";
        out << "} else {\n";
        out << "    for (double i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_" << node_idx
            << ") {\n";
        emit_for_body();
        out << "    }\n";
        out << "}\n";
    }

    for (size_t _hi = 0; _hi < state.hoisted_lookups.size(); ++_hi)
        out << "L->shadow_top--;\n";
    state.hoisted_lookups = std::move(_saved_hoisted);
    state.hoisted_cfuncs = std::move(_saved_hoisted_cf);
    out << "}\n";
}

//------------------ emitGenericForStatement: handles NodeType::GenericForStatement
void CodeEmitter::emitGenericForStatement(const ASTNode &node, uint32_t node_idx) {
    out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
    const auto &loop = node.as.generic_for;

    out << "{\n    clx::ScopeGuard _sg_gen_for_" << node_idx << "(L);\n";

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

        out << "    {\n";
        if (call_node.as.call_expr.arg_count > 0) {
            out << "        clx::LValue args_" << iter_node << "[] = {";
            for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                if (a < call_node.as.call_expr.arg_count - 1)
                    out << ", ";
            }
            out << "};\n";
        }

        if (is_direct) {
            out << "        _triplet_" << node_idx << " = _impl_" << fname << "(L, "
                << (call_node.as.call_expr.arg_count > 0 ? "args_" + std::to_string(iter_node) : "nullptr") << ", "
                << call_node.as.call_expr.arg_count << ");\n";
        } else {
            if (call_node.as.call_expr.arg_count > 0) {
                out << "        for (size_t i = 0; i < " << call_node.as.call_expr.arg_count
                    << "; ++i) L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&args_" << iter_node
                    << "[i].val, &args_" << iter_node << "[i].type);\n";
            }
            out << "        _triplet_" << node_idx << " = clx::call_function(L, ";
            emit_node(tgt);
            out << ", " << (call_node.as.call_expr.arg_count > 0 ? "args_" + std::to_string(iter_node) : "nullptr")
                << ", " << call_node.as.call_expr.arg_count << ", \"" << ctx.filename << "\", " << call_node.line
                << ");\n";
            if (call_node.as.call_expr.arg_count > 0) {
                out << "        L->shadow_top -= " << call_node.as.call_expr.arg_count << ";\n";
            }
        }
        out << "    }\n";
    } else if (loop.iter_count > 0) {
        uint32_t iter_node = ctx.block_statements[loop.first_iter];
        out << "    _triplet_" << node_idx << " = clx::MultiValue(";
        emit_node(iter_node);
        out << ");\n";
    } else {
        out << "    _triplet_" << node_idx << " = clx::MultiValue();\n";
    }

    out << "    clx::LValue _f_" << node_idx << " = (_triplet_" << node_idx << ".count > 0) ? _triplet_" << node_idx
        << "[0] : clx::LValue();\n";
    out << "    clx::LValue _s_" << node_idx << " = (_triplet_" << node_idx << ".count > 1) ? _triplet_" << node_idx
        << "[1] : clx::LValue();\n";
    out << "    clx::LValue _var_" << node_idx << " = (_triplet_" << node_idx << ".count > 2) ? _triplet_" << node_idx
        << "[2] : clx::LValue();\n";

    out << "    while (true) {\n";
    out << "        clx::LValue _args_" << node_idx << "[] = { _s_" << node_idx << ", _var_" << node_idx << " };\n";
    out << "        clx::LCFunction* _lcf_" << node_idx << " = static_cast<clx::LCFunction*>(_f_" << node_idx
        << ".as_pointer());\n";
    out << "        clx::MultiValue _res_" << node_idx << " = _lcf_" << node_idx << "->direct ? _lcf_" << node_idx
        << "->direct(L, _args_" << node_idx << ", 2) : _lcf_" << node_idx << "->func(L, _args_" << node_idx
        << ", 2);\n";

    out << "        _var_" << node_idx << " = (_res_" << node_idx << ".count > 0) ? _res_" << node_idx
        << "[0] : clx::LValue();\n";
    out << "        if (_var_" << node_idx << ".type == clx::ValueType::Nil) break;\n";

    size_t prev_locals = locals.size();
    for (uint32_t i = 0; i < loop.var_count; ++i) {
        uint32_t v_idx = ctx.block_statements[loop.first_var + i];
        std::string_view v_name(ctx.nodes[v_idx].as.ident.name, ctx.nodes[v_idx].as.ident.length);
        bool is_cap = ctx.nodes[v_idx].as.ident.is_captured;

        state.native_numbers.erase(
            std::remove(state.native_numbers.begin(), state.native_numbers.end(), v_name), state.native_numbers.end());
        state.native_integers.erase(std::string(v_name));

        if (is_cap) {
            out << "        clx::LUpValue l_" << v_name << ";\n";
            out << "        l_" << v_name << " = clx::make_upvalue((_res_" << node_idx << ".count > " << i
                << ") ? _res_" << node_idx << "[" << i << "] : clx::LValue());\n";
            out << "        L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << v_name << "->val, &l_" << v_name
                << "->type);\n";
        } else {
            out << "        clx::LValue l_" << v_name << " = (_res_" << node_idx << ".count > " << i << ") ? _res_"
                << node_idx << "[" << i << "] : clx::LValue();\n";
            out << "        L->shadow_stack[L->shadow_top++] = clx::TypedSlot(&l_" << v_name << ".val, &l_" << v_name
                << ".type);\n";
        }
        locals.push_back({ v_name, is_cap });
    }

    if (loop.body_block != 0xFFFFFFFF)
        emit_node(loop.body_block);

    locals.resize(prev_locals);
    out << "    }\n}\n";
}

//------------------ emitTableConstructor: handles NodeType::TableConstructor
void CodeEmitter::emitTableConstructor(const ASTNode &node, uint32_t node_idx) {
    bool _has_va = false;
    for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
        uint32_t v = ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1];
        if (ctx.nodes[v].type == NodeType::Vararg || ctx.nodes[v].type == NodeType::CallExpression) {
            _has_va = true;
            break;
        }
    }
    bool _is_arena = state.current_arena_func != 0xFFFFFFFF && state.arena_safe_table_nodes.count(node_idx) > 0;
    out << "([&]() {\nclx::LValue _t = ";
    if (_is_arena) {
        out << "clx::arena_create_table(L, &_arena, ";
    } else {
        out << "L->create_table(";
    }
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
        if (check_declared(check_declared, nidx)) {
            out << "static_cast<size_t>(";
            emit_native(nidx);
            out << ")";
        } else {
            out << node.as.table_cons.count;
        }
    } else {
        out << node.as.table_cons.count;
    }
    if (_is_arena) {
        out << ", 0);\n";
    } else {
        out << ");\nL->shadow_stack[L->shadow_top++] = clx::TypedSlot(&_t.val, &_t.type);\n";
    }
    if (_has_va)
        out << "size_t _ai = 1;\n";
    uint32_t array_index = 1;

    for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
        uint32_t k = ctx.block_statements[node.as.table_cons.first_item + i * 2];
        uint32_t v = ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1];
        if (i == node.as.table_cons.count - 1
            && (ctx.nodes[v].type == NodeType::Vararg || ctx.nodes[v].type == NodeType::CallExpression)) {
            state.expect_multivalue = true;
            out << "clx::MultiValue _mret_" << node_idx << " = ";
            emit_node(v);
            out << ";\n";
            state.expect_multivalue = false;
            out << "for (size_t _vi = 0; _vi < _mret_" << node_idx << ".count; ++_vi) {\n";
            out << "  static_cast<clx::LTable*>(_t.as_pointer())->settable(";
            out << "clx::LValue(static_cast<double>(_ai++)), _mret_" << node_idx << "[_vi]);\n";
            out << "}\n";
        } else {
            out << "static_cast<clx::LTable*>(_t.as_pointer())->settable(";
            if (k == 0xFFFFFFFF) {
                out << "clx::LValue(static_cast<double>(" << (_has_va ? "_ai++" : std::to_string(array_index++))
                    << "))";
            } else {
                emit_node(k);
            }
            out << ", ";
            emit_node(v);
            out << ");\n";
        }
    }
    if (!_is_arena)
        out << "L->shadow_top--;\n";
    out << "return _t;\n}())";
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
        out << "clx::LValue(static_cast<double>(l_" << t_name << "[static_cast<size_t>(";
        emit_native(node.as.table_access.key);
        out << ") - 1]))";
        return;
    }
    if (state.bce_safe_nodes.count(node_idx)) {
        out << "([&](){ auto* _tb" << node_idx << " = static_cast<clx::LTable*>((";
        emit_node(node.as.table_access.table);
        out << ").as_pointer()); size_t _ki" << node_idx << " = static_cast<size_t>(";
        emit_native(node.as.table_access.key);
        out << "); return clx::LValue(_tb" << node_idx << "->array[_ki" << node_idx << " - 1], _tb" << node_idx
            << "->array_types[_ki" << node_idx << " - 1]); }())";
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
                out << "([&](){ clx::LTable* _tc = static_cast<clx::LTable*>((";
                emit_node(_cb);
                out << ").as_pointer());";
                for (size_t _i = 0; _i < _cks.size() - 1; ++_i) {
                    out << " size_t _k" << _i << " = static_cast<size_t>(";
                    emit_native(_cks[_i]);
                    out << ");";
                    out << " clx::LValue _v" << _i << " = (_k" << _i
                        << " - 1 < _tc->array_size) ? clx::LValue(_tc->array[_k" << _i << " - 1], _tc->array_types[_k"
                        << _i << " - 1]) : clx::table_get_int(L, clx::LValue(clx::ValueType::Table, _tc), _k" << _i
                        << ");";
                    out << " _tc = static_cast<clx::LTable*>(_v" << _i << ".as_pointer());";
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
                out << "([&](){ clx::LTable* _t = static_cast<clx::LTable*>((";
                emit_node(node.as.table_access.table);
                out << ").as_pointer()); size_t _k = static_cast<size_t>(";
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
                    << (std::distance(state.string_pool.begin(),
                           std::find(state.string_pool.begin(), state.string_pool.end(),
                               std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length))))
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
        out << "clx::MultiValue(_va_args, _va_count)";
    else
        out << "(_va_count > 0 ? _va_args[0] : clx::LValue())";
}

//------------------ emitBreakStatement: handles NodeType::BreakStatement
void CodeEmitter::emitBreakStatement(const ASTNode &node, uint32_t node_idx) {
    out << "break;";
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
