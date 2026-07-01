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

using clx::yields_number;


namespace clx {


std::vector<std::string_view> CodeEmitter::g_native_numbers;
std::vector<std::string_view> CodeEmitter::g_string_pool;
std::set<std::string_view> CodeEmitter::g_native_return_funcs;
std::set<std::string> CodeEmitter::g_param_numbers;
std::vector<std::string> CodeEmitter::g_param_names;
std::map<uint32_t, uint32_t> CodeEmitter::g_table_presize;
std::map<std::string_view, double> CodeEmitter::g_global_constants;
std::set<uint32_t> CodeEmitter::g_bce_safe_nodes;
std::set<std::string_view> CodeEmitter::g_pure_numeric_arrays;
std::map<std::string_view, std::set<std::string_view>> CodeEmitter::g_numeric_table_fields;

std::set<std::string_view> CodeEmitter::g_direct_callables;
std::set<std::string_view> CodeEmitter::g_native_integers;
std::set<std::string_view> CodeEmitter::g_fast_callables;
std::map<std::string_view, uint32_t> CodeEmitter::g_func_param_counts;
std::map<std::string_view, std::vector<bool>> CodeEmitter::g_func_param_native;

bool CodeEmitter::g_emit_raw_lambda = false;
bool CodeEmitter::g_emit_fast_lambda = false;
bool CodeEmitter::g_in_function_def = false;
std::string CodeEmitter::g_ref_capture;
bool CodeEmitter::g_in_fast_function = false;
bool CodeEmitter::g_expect_multivalue = false;
std::string_view CodeEmitter::g_current_fast_func = "";
std::set<std::string_view> CodeEmitter::g_reassigned_vars;
std::set<std::string_view> CodeEmitter::g_constant_upvalues;
std::set<std::string_view> CodeEmitter::g_string_builders;
std::set<std::string_view> CodeEmitter::g_global_string_builders;
std::set<std::string_view> CodeEmitter::g_module_string_builders;
std::map<uint32_t, uint32_t> CodeEmitter::g_goto_targets;
std::set<std::string_view> CodeEmitter::g_hoisted_locals;
bool CodeEmitter::g_skip_block_braces = false;
std::unordered_map<uint32_t, std::string> CodeEmitter::g_hoisted_lookups;
std::unordered_map<std::string, std::string> CodeEmitter::g_hoisted_cfuncs;
std::unordered_map<std::string, std::string> CodeEmitter::g_builtin_aliases;
int CodeEmitter::g_cs_index = 0;


//------------------ lookup_builtin: maps "module.func" to C++ function name
const char* lookup_builtin(std::string_view module, std::string_view func) {
    static const std::unordered_map<std::string_view, std::unordered_map<std::string_view, const char*>> _sfm = {
        {"string", {
            {"byte", "str_byte"}, {"sub", "str_sub"}, {"match", "str_match"},
            {"find", "str_find"}, {"gsub", "str_gsub"}, {"len", "str_len"},
            {"format", "str_format"}, {"char", "str_char"}, {"rep", "str_rep"},
            {"reverse", "str_reverse"}, {"lower", "str_lower"}, {"upper", "str_upper"},
            {"dump", "str_dump"}
        }},
        {"table", {
            {"concat", "table_concat"}, {"insert", "table_insert"},
            {"remove", "table_remove"}, {"sort", "table_sort"},
            {"pack", "table_pack"}, {"unpack", "table_unpack"}, {"move", "table_move"}
        }},
        {"_G", {
            {"type", "__clx_type"}, {"tostring", "__clx_tostring"}
        }}
    };
    auto _smit = _sfm.find(module);
    if (_smit != _sfm.end()) {
        auto _sfit = _smit->second.find(func);
        if (_sfit != _smit->second.end())
            return _sfit->second;
    }
    return nullptr;
}


//------------------ CodeEmitter: constructor initializes output stream with precision
CodeEmitter::CodeEmitter(const ASTContext& context, const char* output_path) : ctx(context), out(output_path) {
    out << std::setprecision(17);
}

//------------------ is_local: checks if variable is local in current scope
bool CodeEmitter::is_local(std::string_view name, bool& out_is_boxed) {
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (it->name == name) {
            out_is_boxed = it->is_boxed;
            return true;
        }
    }
    return false;
}


//------------------ lua_decode_string: decodes Lua string escape sequences
static std::string lua_decode_string(std::string_view s) {
    std::string r;
    r.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\\' && i + 1 < s.length()) {
            unsigned char c = static_cast<unsigned char>(s[i + 1]);
            switch (c) {
                case 'a':  r += '\a'; i++; break;
                case 'b':  r += '\b'; i++; break;
                case 'f':  r += '\f'; i++; break;
                case 'n':  r += '\n'; i++; break;
                case 'r':  r += '\r'; i++; break;
                case 't':  r += '\t'; i++; break;
                case 'v':  r += '\v'; i++; break;
                case '\\': r += '\\'; i++; break;
                case '"':  r += '\"'; i++; break;
                case '\'': r += '\''; i++; break;
                case 'x': {
                    if (i + 3 < s.length()) {
                        char hex[3] = {s[i+2], s[i+3], 0};
                        char* end;
                        long val = std::strtol(hex, &end, 16);
                        if (end == hex + 2) {
                            r += static_cast<char>(val);
                            i += 3;
                        } else { r += s[i]; }
                    } else { r += s[i]; }
                    break;
                }
                case 'u': {
                    if (i + 2 < s.length() && s[i+2] == '{') {
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
                        } else { r += s[i]; }
                    } else { r += s[i]; }
                    break;
                }
                default: {

                    if (c >= '0' && c <= '9') {
                        size_t j = i + 1;
                        while (j < s.length() && j - i <= 3 && s[j] >= '0' && s[j] <= '7') j++;
                        if (j > i + 1) {
                            std::string oct(s.data() + i + 1, j - i - 1);
                            long val = std::strtol(oct.c_str(), nullptr, 8);
                            r += static_cast<char>(val);
                            i = j - 1;
                        } else { r += s[i]; }
                    } else { r += s[i]; }
                    break;
                }
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

//------------------ cpp_escape: escapes C++ special characters in a string
static std::string cpp_escape(std::string_view s) {
    std::string r;
    r.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\a': r += "\\a";  break;
            case '\b': r += "\\b";  break;
            case '\f': r += "\\f";  break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            case '\v': r += "\\v";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    bool next_is_hex = (i + 1 < s.length() &&
                        ((s[i+1] >= '0' && s[i+1] <= '9') ||
                         (s[i+1] >= 'a' && s[i+1] <= 'f') ||
                         (s[i+1] >= 'A' && s[i+1] <= 'F')));
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

//------------------ emit: main entry point for C++ code generation
void CodeEmitter::emit(uint32_t root_node, std::string_view module_name) {
    CodeEmitter::g_native_numbers.clear();
    CodeEmitter::g_string_pool.clear();
    CodeEmitter::g_table_presize.clear();
    CodeEmitter::g_global_constants.clear();
    CodeEmitter::g_bce_safe_nodes.clear();
    CodeEmitter::g_direct_callables.clear();
    CodeEmitter::g_fast_callables.clear();
    CodeEmitter::g_native_return_funcs.clear();
    CodeEmitter::g_func_param_counts.clear();
    CodeEmitter::g_param_numbers.clear();
    CodeEmitter::g_param_names.clear();
    CodeEmitter::g_func_param_native.clear();
    CodeEmitter::g_reassigned_vars.clear();
    CodeEmitter::g_constant_upvalues.clear();
    CodeEmitter::g_string_builders.clear();
    CodeEmitter::g_global_string_builders.clear();
    CodeEmitter::g_module_string_builders.clear();
    CodeEmitter::g_goto_targets.clear();
    CodeEmitter::g_hoisted_locals.clear();
    CodeEmitter::g_pure_numeric_arrays.clear();
    CodeEmitter::g_numeric_table_fields.clear();
    CodeEmitter::g_native_integers.clear();
    CodeEmitter::g_emit_raw_lambda = false;
    CodeEmitter::g_emit_fast_lambda = false;
    CodeEmitter::g_in_fast_function = false;
    CodeEmitter::g_in_function_def = false;
    CodeEmitter::g_current_fast_func = "";
    CodeEmitter::g_ref_capture.clear();

    Optimizer::run(ctx, root_node);



    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto& node = ctx.nodes[i];
        bool is_module_level = false;
        if (root_node < ctx.nodes.size() && ctx.nodes[root_node].type == NodeType::Block) {
            const auto& rb = ctx.nodes[root_node].as.block;
            for (uint32_t j = 0; j < rb.count; ++j) {
                if (ctx.block_statements[rb.first_statement + j] == i) { is_module_level = true; break; }
            }
        }
        if (node.type == NodeType::Assignment && node.as.assign.target_count == 1 && node.as.assign.value_count == 1) {
            uint32_t t_idx = ctx.block_statements[node.as.assign.first_target];
            uint32_t v_idx = ctx.block_statements[node.as.assign.first_value];
            if (ctx.nodes[t_idx].type == NodeType::Identifier) {
                std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                const auto& v_node = ctx.nodes[v_idx];
                if (v_node.type == NodeType::BinaryOp && v_node.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {

                    std::vector<uint32_t> ops;
                    std::vector<uint32_t> ws;
                    ws.push_back(v_idx);
                    while (!ws.empty()) {
                        uint32_t cur = ws.back(); ws.pop_back();
                        const auto& cn = ctx.nodes[cur];
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
                            CodeEmitter::g_string_builders.insert(name);
                            if (ctx.nodes[t_idx].as.ident.is_global) {
                                CodeEmitter::g_global_string_builders.insert(name);
                            } else if (is_module_level) {
                                CodeEmitter::g_module_string_builders.insert(name);
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
            const auto& node = ctx.nodes[i];
            if (node.type != NodeType::BinaryOp || node.as.bin_op.op != 13) continue;
            std::vector<uint32_t> ws;
            std::vector<uint32_t> ops;
            ws.push_back(i);
            while (!ws.empty()) {
                uint32_t cur = ws.back(); ws.pop_back();
                const auto& cn = ctx.nodes[cur];
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
            const auto& node = ctx.nodes[i];
            if (node.type != NodeType::LocalDecl) continue;
            bool is_module_level = false;
            if (root_node < ctx.nodes.size() && ctx.nodes[root_node].type == NodeType::Block) {
                const auto& rb = ctx.nodes[root_node].as.block;
                for (uint32_t j = 0; j < rb.count; ++j) {
                    if (ctx.block_statements[rb.first_statement + j] == i) { is_module_level = true; break; }
                }
            }
            if (!is_module_level) continue;
            for (uint32_t ii = 0; ii < node.as.local_decl.ident_count; ++ii) {
                uint32_t idi = ctx.block_statements[node.as.local_decl.first_ident + ii];
                if (ctx.nodes[idi].type != NodeType::Identifier) continue;
                std::string_view nm(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                if (concat_idents.count(nm) && !ctx.nodes[idi].as.ident.is_captured) {
                    CodeEmitter::g_string_builders.insert(nm);
                    CodeEmitter::g_module_string_builders.insert(nm);
                }
            }
        }
    }

    out << "#include <clx.h>\n";
    out << "#include <atomic>\n\n";

    if (!CodeEmitter::g_string_pool.empty()) {
        out << "static clx::LValue cstr_[" << CodeEmitter::g_string_pool.size() << "];\n";
    }
    out << "static inline clx::LValue clx_gettable_safe(clx::LValue* ptr) { return ptr ? *ptr : clx::LValue(); }\n\n";

    out << "CLX_API clx::LValue luaopen_" << module_name << "(clx::LState* L) {\n";
    out << "    clx::LValue _ENV(clx::LType::Table, L->_G);\n";
    out << "    clx::CacheSlot __cs[" << CodeEmitter::g_cs_max << "]{};\n";
    CodeEmitter::g_cs_index = 0;


    for (const auto& sb_name : CodeEmitter::g_global_string_builders) {
        out << "    clx::StringBuilder sb_" << sb_name << ";\n";
    }
    for (const auto& sb_name : CodeEmitter::g_module_string_builders) {
        out << "    clx::StringBuilder sb_" << sb_name << ";\n";
    }


    if (!CodeEmitter::g_string_pool.empty()) {
        size_t n = CodeEmitter::g_string_pool.size();
        size_t cap = 64;
        while (cap < n * 2) cap *= 2;
        out << "    L->string_pool.reserve(" << cap << ");\n";
    }

    if (!CodeEmitter::g_string_pool.empty()) {
        size_t n = CodeEmitter::g_string_pool.size();
        out << "    static const struct { const char* s; unsigned int len; unsigned int hash; } _cstr_data[" << n << "] = {\n";
        for (size_t i = 0; i < n; ++i) {
            auto& s = CodeEmitter::g_string_pool[i];
            std::string decoded = lua_decode_string(s);
            out << "        {\"" << cpp_escape(decoded) << "\", "
                << decoded.length() << ", "
                << (decoded.length() <= 8 ? swar_hash_8(decoded.data(), decoded.length()) : wyhash_str(decoded.data(), decoded.length()))
                << "U},\n";
        }
        out << "    };\n";
        out << "    for (size_t _i = 0; _i < " << n << "; ++_i)\n";
        out << "        cstr_[_i] = clx::LValue(L->intern_prehashed(_cstr_data[_i].s, _cstr_data[_i].len, _cstr_data[_i].hash));\n";
    }

    if (!ctx.nodes.empty()) emit_node(root_node);
    out << "    return clx::LValue();\n";
    out << "}\n";
}


//------------------ emit_node: dispatches C++ code generation per AST node type
void CodeEmitter::emit_node(uint32_t node_idx) {
    if (node_idx >= ctx.nodes.size()) return;
    const ASTNode& node = ctx.nodes[node_idx];

    auto emit_native = [&](auto& self, uint32_t n_idx) -> void {
        if (yields_number(ctx, n_idx, nullptr, CodeEmitter::g_current_fast_func)) {
            const auto& n = ctx.nodes[n_idx];
            if (n.type == NodeType::IntrinsicCall) {
                const char* _cn = n.as.intrinsic_call.cname;
                if (strcmp(_cn, "__clx_deg") == 0 || strcmp(_cn, "__clx_rad") == 0) {
                    if (n.as.intrinsic_call.arg_count > 0) {
                        self(self, ctx.block_statements[n.as.intrinsic_call.first_arg]);
                        out << (_cn[7] == 'd' ? " * 57.29577951308232" : " * 0.017453292519943295");
                    } else out << "0.0";
                    return;
                }
                if (strcmp(_cn, "std::log") == 0 && n.as.intrinsic_call.arg_count > 1) {
                    out << "std::log(";
                    self(self, ctx.block_statements[n.as.intrinsic_call.first_arg]);
                    out << ") / std::log(";
                    self(self, ctx.block_statements[n.as.intrinsic_call.first_arg + 1]);
                    out << ")";
                    return;
                }
                out << _cn << "(";
                if (n.as.intrinsic_call.arg_count > 0) {
                    self(self, ctx.block_statements[n.as.intrinsic_call.first_arg]);
                    if (n.as.intrinsic_call.arg_count > 1 && (strcmp(_cn, "std::fmod") == 0 || strcmp(_cn, "std::atan2") == 0 || strcmp(_cn, "std::pow") == 0)) {
                        out << ", ";
                        self(self, ctx.block_statements[n.as.intrinsic_call.first_arg + 1]);
                    }
                } else out << "0.0";
                out << ")";
                return;
            }
            if (n.type == NodeType::Number) { out << n.as.number.val; return; }
            if (n.type == NodeType::Integer) { out << n.as.integer.val << ".0"; return; }
            if (n.type == NodeType::Identifier) {
                std::string_view name(n.as.ident.name, n.as.ident.length);
                if (CodeEmitter::g_global_constants.count(name)) { out << CodeEmitter::g_global_constants[name]; return; }
                 if (std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end()) {
                    bool is_boxed = false;
                    this->is_local(name, is_boxed);
                    if (is_boxed) out << "(*l_" << name << ").as_number()";
                    else out << "l_" << name;
                    return;
                }
            }
            if (n.type == NodeType::BinaryOp) {
                int op = n.as.bin_op.op;
                auto is_int_val = [&](auto& self, uint32_t idx) -> bool {
                    const auto& nn = ctx.nodes[idx];
                    if (nn.type == NodeType::Integer) return true;
                    if (nn.type == NodeType::Number) {
                        double v = nn.as.number.val;
                        return v == static_cast<double>(static_cast<int64_t>(v)) &&
                               v >= -9007199254740992.0 && v <= 9007199254740992.0;
                    }
                    if (nn.type == NodeType::Identifier) {
                        std::string_view nm(nn.as.ident.name, nn.as.ident.length);
                        return CodeEmitter::g_native_integers.count(nm) > 0;
                    }
                    if (nn.type == NodeType::BinaryOp) {
                        int bop = nn.as.bin_op.op;
                        if (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
                            return self(self, nn.as.bin_op.left) && self(self, nn.as.bin_op.right);
                    }
                    return false;
                };
                if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Mul) && is_int_val(is_int_val, n.as.bin_op.left) && is_int_val(is_int_val, n.as.bin_op.right)) {
                    out << "static_cast<double>(static_cast<int64_t>("; self(self, n.as.bin_op.left); out << ")";
                    if (op==1) out<<" + "; if (op==2) out<<" - "; if (op==3) out<<" * ";
                    out << "static_cast<int64_t>("; self(self, n.as.bin_op.right); out << "))";
                    return;
                }
                if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) {
                    out << "("; self(self, n.as.bin_op.left);
                    if (op==1) out<<" + "; if (op==2) out<<" - "; if (op==3) out<<" * "; if (op==4) out<<" / ";
                    self(self, n.as.bin_op.right); out << ")";
                    return;
                }
                if (op == static_cast<int>(BinaryOp::Mod)) {
                    out << "std::fmod("; self(self, n.as.bin_op.left); out << ", "; self(self, n.as.bin_op.right); out << ")";
                    return;
                }
                if (op == static_cast<int>(BinaryOp::FloorDiv)) {
                    out << "std::floor(("; self(self, n.as.bin_op.left); out << ") / ("; self(self, n.as.bin_op.right); out << "))";
                    return;
                }
                if (op == static_cast<int>(BinaryOp::Mod)) {
                    auto si = [&](uint32_t idx) -> bool {
                        const auto& nn = ctx.nodes[idx];
                        if (nn.type == NodeType::Integer) return true;
                        if (nn.type == NodeType::Number) {
                            double v = nn.as.number.val;
                            return v == static_cast<double>(static_cast<int64_t>(v)) && v >= -9.0e15 && v <= 9.0e15;
                        }
                        if (nn.type == NodeType::Identifier) {
                            std::string_view nm(nn.as.ident.name, nn.as.ident.length);
                            return CodeEmitter::g_native_integers.count(nm) > 0;
                        }
                        return false;
                    };
                    if (si(n.as.bin_op.left) && si(n.as.bin_op.right)) {
                        out << "static_cast<double>(static_cast<int64_t>("; self(self, n.as.bin_op.left); out << ") % static_cast<int64_t>("; self(self, n.as.bin_op.right); out << "))";
                    } else {
                        out << "std::fmod("; self(self, n.as.bin_op.left); out << ", "; self(self, n.as.bin_op.right); out << ")";
                    }
                    return;
                }
            }
            if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Minus)) {
                out << "(-("; self(self, n.as.unary_op.expr); out << "))";
                return;
            }
            if (n.type == NodeType::ParenExpression) {
                out << "("; self(self, n.as.paren_expr.expr); out << ")";
                return;
            }
            if (n.type == NodeType::TableAccess) {
                {
                    auto hit = CodeEmitter::g_hoisted_lookups.find(n_idx);
                    if (hit != CodeEmitter::g_hoisted_lookups.end()) {
                        out << "(" << hit->second << ").as_number()";
                        return;
                    }
                }
                if (ctx.nodes[n.as.table_access.key].type == NodeType::String) {
                    return;
                }
                std::string_view t_name;
                if (ctx.nodes[n.as.table_access.table].type == NodeType::Identifier) {
                    t_name = std::string_view(ctx.nodes[n.as.table_access.table].as.ident.name, ctx.nodes[n.as.table_access.table].as.ident.length);
                }
                if (!t_name.empty() && CodeEmitter::g_pure_numeric_arrays.count(t_name)) {
                    out << "l_" << t_name << "[static_cast<size_t>("; self(self, n.as.table_access.key); out << ") - 1]";
                    return;
                }
                if (CodeEmitter::g_bce_safe_nodes.count(n_idx)) {
                     out << "(static_cast<clx::LTable*>(("; emit_node(n.as.table_access.table); out << ").as_pointer())->array[static_cast<size_t>("; self(self, n.as.table_access.key); out << ") - 1]).as_number()";
                     return;
                }
                bool pure_t = ctx.nodes[n.as.table_access.table].type == NodeType::Identifier;
                if (pure_t) {
                     out << "((static_cast<size_t>("; self(self, n.as.table_access.key); out << ") - 1 < static_cast<clx::LTable*>(("; emit_node(n.as.table_access.table); out << ").as_pointer())->array_size) ? static_cast<clx::LTable*>(("; emit_node(n.as.table_access.table); out << ").as_pointer())->array[static_cast<size_t>("; self(self, n.as.table_access.key); out << ") - 1].as_number() : clx::table_get_int(L, "; emit_node(n.as.table_access.table); out << ", static_cast<size_t>("; self(self, n.as.table_access.key); out << ")).as_number())";
                     return;
                }
            }
            if (n.type == NodeType::CallExpression) {
                bool is_fast = false;
                std::string_view fname;
                uint32_t tgt = n.as.call_expr.target;
                if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                    fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                    if (CodeEmitter::g_fast_callables.count(fname) || (!CodeEmitter::g_current_fast_func.empty() && fname == CodeEmitter::g_current_fast_func)) is_fast = true;
                }
                if (is_fast) {
                    if (fname == CodeEmitter::g_current_fast_func) {
                        out << "self(self";
                        if (n.as.call_expr.arg_count > 0 || CodeEmitter::g_func_param_counts[fname] > 0) out << ", ";
                    } else {
                        out << "_fast_" << fname << "(";
                    }
                    for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) {
                        self(self, ctx.block_statements[n.as.call_expr.first_arg + i]);
                        if (i < n.as.call_expr.arg_count - 1) out << ", ";
                    }
                    for (uint32_t i = n.as.call_expr.arg_count; i < CodeEmitter::g_func_param_counts[fname]; ++i) {
                        if (i > 0 || n.as.call_expr.arg_count > 0) out << ", ";
                        out << "0.0";
                    }
                    out << ")";
                    return;
                }
            }
        }
        out << "("; emit_node(n_idx); out << ").as_number()";
    };

    auto emit_condition = [&](auto& self, uint32_t c_idx) -> void {
        if (c_idx == 0xFFFFFFFF || c_idx >= ctx.nodes.size()) {
            out << "false"; return;
        }
        const auto& c = ctx.nodes[c_idx];
        if (c.type == NodeType::TableAccess) {
            std::string_view t_name;
            if (ctx.nodes[c.as.table_access.table].type == NodeType::Identifier) {
                t_name = std::string_view(ctx.nodes[c.as.table_access.table].as.ident.name, ctx.nodes[c.as.table_access.table].as.ident.length);
            }
            if (!t_name.empty() && CodeEmitter::g_pure_numeric_arrays.count(t_name)) {
                out << "(l_" << t_name << "[static_cast<size_t>("; emit_native(emit_native, c.as.table_access.key); out << ") - 1] != 0.0)";
                return;
            }
        }
        if (c.type == NodeType::BinaryOp) {
            int op = c.as.bin_op.op;
            if (op == static_cast<int>(BinaryOp::And) || op == static_cast<int>(BinaryOp::Or)) {
                out << "(";
                self(self, c.as.bin_op.left);
                out << (op == static_cast<int>(BinaryOp::And) ? " && " : " || ");
                self(self, c.as.bin_op.right);
                out << ")";
                return;
            }
            if (op >= static_cast<int>(BinaryOp::Eq) && op <= static_cast<int>(BinaryOp::Ne)) {
                bool left_native = yields_number(ctx, c.as.bin_op.left, nullptr, CodeEmitter::g_current_fast_func);
                bool right_native = yields_number(ctx, c.as.bin_op.right, nullptr, CodeEmitter::g_current_fast_func);
                if (left_native && right_native) {
                    static const char* ops[] = {"", "", "", "", "", " == ", " < ", " > ", " <= ", " >= ", " != "};
                    out << "("; emit_native(emit_native, c.as.bin_op.left); out << ops[op]; emit_native(emit_native, c.as.bin_op.right); out << ")";
                    return;
                }
            }
        }
        if (c.type == NodeType::UnaryOp && c.as.unary_op.op == static_cast<int>(UnaryOp::Not)) {
            out << "!(";
            self(self, c.as.unary_op.expr);
            out << ")";
            return;
        }
        if (c.type == NodeType::ParenExpression) {
            out << "("; self(self, c.as.paren_expr.expr); out << ")";
            return;
        }
        out << "("; emit_node(c_idx); out << ").as_bool()";
    };

    switch (node.type) {
        //------------------ NodeType::IntrinsicCall: emits an intrinsic call
        case NodeType::IntrinsicCall: {
            const char* _cn = node.as.intrinsic_call.cname;
            if (strcmp(_cn, "__clx_type") == 0) {
                uint32_t va = ctx.block_statements[node.as.intrinsic_call.first_arg];
                out << "([&](){ static const char* _tn[]={\"number\",\"nil\",\"boolean\",\"number\",\"string\",\"table\",\"function\",\"userdata\",\"thread\"}; return clx::LValue(L->intern_string(_tn[static_cast<uint8_t>((";
                emit_node(va);
                out << ").type())])); }())";
            } else if (strcmp(_cn, "__clx_tostring") == 0) {
                uint32_t va = ctx.block_statements[node.as.intrinsic_call.first_arg];
                out << "clx::LValue(L->intern_string((";
                emit_node(va);
                out << ").to_string(L)))";
            } else {
                out << "clx::LValue(static_cast<double>(";
                if (strcmp(_cn, "__clx_deg") == 0 || strcmp(_cn, "__clx_rad") == 0) {
                    emit_native(emit_native, ctx.block_statements[node.as.intrinsic_call.first_arg]);
                    out << (_cn[7] == 'd' ? " * 57.29577951308232" : " * 0.017453292519943295");
                } else if (strcmp(_cn, "std::log") == 0 && node.as.intrinsic_call.arg_count > 1) {
                    out << "std::log(";
                    emit_native(emit_native, ctx.block_statements[node.as.intrinsic_call.first_arg]);
                    out << ") / std::log(";
                    emit_native(emit_native, ctx.block_statements[node.as.intrinsic_call.first_arg + 1]);
                    out << ")";
                } else {
                    out << _cn << "(";
                    if (node.as.intrinsic_call.arg_count > 0) {
                        emit_native(emit_native, ctx.block_statements[node.as.intrinsic_call.first_arg]);
                        if (node.as.intrinsic_call.arg_count > 1 && (strcmp(_cn, "std::fmod") == 0 || strcmp(_cn, "std::atan2") == 0 || strcmp(_cn, "std::pow") == 0)) {
                            out << ", ";
                            emit_native(emit_native, ctx.block_statements[node.as.intrinsic_call.first_arg + 1]);
                        }
                    } else {
                        out << "0.0";
                    }
                    out << ")";
                }
                out << "))";
            }
            break;
        }

        //------------------ NodeType::CallExpression: emits a function call expression
        case NodeType::CallExpression: {
            bool is_direct = false;
            bool is_fast = false;
            std::string_view fname;
            uint32_t tgt = node.as.call_expr.target;

            bool is_method_call = false;
            if (ctx.nodes[tgt].type == NodeType::TableAccess) {
                if (node.as.call_expr.arg_count > 0 && ctx.block_statements[node.as.call_expr.first_arg] == ctx.nodes[tgt].as.table_access.table) {
                    is_method_call = true;
                }
            }

            if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                if (CodeEmitter::g_fast_callables.count(fname) || (!CodeEmitter::g_current_fast_func.empty() && fname == CodeEmitter::g_current_fast_func)) is_fast = true;
                else if (CodeEmitter::g_direct_callables.count(fname)) is_direct = true;
            }

            bool want_multi = CodeEmitter::g_expect_multivalue;
            CodeEmitter::g_expect_multivalue = false;

            if (is_fast) {
                if (want_multi) out << "clx::MultiValue(";
                out << "clx::LValue(static_cast<double>(";
                if (fname == CodeEmitter::g_current_fast_func) {
                    out << "self(self";
                    if (node.as.call_expr.arg_count > 0 || CodeEmitter::g_func_param_counts[fname] > 0) out << ", ";
                } else {
                    out << "_fast_" << fname << "(";
                }

                for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i) {
                    emit_native(emit_native, ctx.block_statements[node.as.call_expr.first_arg + i]);
                    if (i < node.as.call_expr.arg_count - 1) out << ", ";
                }
                for (uint32_t i = node.as.call_expr.arg_count; i < CodeEmitter::g_func_param_counts[fname]; ++i) {
                    if (i > 0 || node.as.call_expr.arg_count > 0) out << ", ";
                    out << "0.0";
                }
                out << ")))";
                if (want_multi) out << ")";
                break;
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
                out << "    clx::LValue _m_self = "; emit_node(ctx.nodes[tgt].as.table_access.table); out << ";\n";
                out << "    L->shadow_stack[L->shadow_top++] = &_m_self;\n";

                bool key_is_native = false;
                uint32_t k_idx = ctx.nodes[tgt].as.table_access.key;
                if (yields_number(ctx, k_idx, nullptr, CodeEmitter::g_current_fast_func)) key_is_native = true;

                out << "    clx::LValue _m_func;\n";
                if (key_is_native) {
                    out << "    _m_func = ([&](){ clx::LTable* _t = static_cast<clx::LTable*>(_m_self.as_pointer()); size_t _k = static_cast<size_t>("; emit_native(emit_native, k_idx); out << "); return (_k - 1 < _t->array_size) ? _t->array[_k - 1] : clx::table_get_int(L, clx::LValue(clx::LType::Table, _t), _k); }());\n";
                } else {
                    int _cs_i = (CodeEmitter::g_cs_index < CodeEmitter::g_cs_max) ? CodeEmitter::g_cs_index++ : -1;
                    if (_cs_i >= 0) {
                        out << "    _m_func = clx::table_get_cs(L, _m_self, "; emit_node(k_idx); out << ", &__cs[" << _cs_i << "]);\n";
                    } else {
                        out << "    _m_func = clx::table_get(L, _m_self, "; emit_node(k_idx); out << ");\n";
                    }
                }
                out << "    L->shadow_stack[L->shadow_top++] = &_m_func;\n";
            }

            if (last_expands) {
                out << "    std::vector<clx::LValue> _dyn_args;\n";
                if (node.as.call_expr.arg_count > 1) {
                    out << "    _dyn_args.reserve(" << (node.as.call_expr.arg_count - 1) << ");\n";
                    for (uint32_t i = 0; i < node.as.call_expr.arg_count - 1; ++i) {
                        out << "    _dyn_args.push_back(";
                        if (is_method_call && i == 0) out << "_m_self";
                        else emit_node(ctx.block_statements[node.as.call_expr.first_arg + i]);
                        out << ");\n";
                    }
                }

                CodeEmitter::g_expect_multivalue = true;
                out << "    clx::MultiValue _mret = ";
                emit_node(last_arg);
                out << ";\n";
                CodeEmitter::g_expect_multivalue = false;

                out << "    for (size_t _mi = 0; _mi < _mret.count; ++_mi) _dyn_args.push_back(_mret[_mi]);\n";

                if (is_direct) {
                    out << "    clx::MultiValue _main_ret = _impl_" << fname << "(L, _dyn_args.data(), _dyn_args.size());\n";
                } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size() && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
                    auto _chit = CodeEmitter::g_hoisted_lookups.find(node.as.call_expr.target);
                    if (_chit != CodeEmitter::g_hoisted_lookups.end()) {
                        auto _cf_it = CodeEmitter::g_hoisted_cfuncs.find(_chit->second);
                        if (_cf_it != CodeEmitter::g_hoisted_cfuncs.end()) {
                            out << "    for (size_t i = 0; i < _dyn_args.size(); ++i) L->shadow_stack[L->shadow_top++] = &_dyn_args[i];\n";
                            out << "    clx::MultiValue _main_ret = clx::" << _cf_it->second << "(L, _dyn_args.data(), _dyn_args.size());\n";
                            out << "    L->shadow_top -= _dyn_args.size();\n";
                        } else { goto _call_direct_dyn; }
                    } else { goto _call_direct_dyn; }
                } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size() && ctx.nodes[node.as.call_expr.target].type == NodeType::Identifier && !ctx.nodes[node.as.call_expr.target].as.ident.is_global) {
                    std::string_view _alias_nm(ctx.nodes[node.as.call_expr.target].as.ident.name, ctx.nodes[node.as.call_expr.target].as.ident.length);
                    auto _alias_it = CodeEmitter::g_builtin_aliases.find(std::string(_alias_nm));
                    if (_alias_it != CodeEmitter::g_builtin_aliases.end() && CodeEmitter::g_reassigned_vars.count(std::string(_alias_nm)) == 0) {
                        out << "    for (size_t i = 0; i < _dyn_args.size(); ++i) L->shadow_stack[L->shadow_top++] = &_dyn_args[i];\n";
                        out << "    clx::MultiValue _main_ret = clx::" << _alias_it->second << "(L, _dyn_args.data(), _dyn_args.size());\n";
                        out << "    L->shadow_top -= _dyn_args.size();\n";
                    } else { goto _call_direct_dyn; }
                } else { _call_direct_dyn:;
                    out << "    for (size_t i = 0; i < _dyn_args.size(); ++i) L->shadow_stack[L->shadow_top++] = &_dyn_args[i];\n";
                    out << "    clx::MultiValue _main_ret = clx::call_direct(L, ";
                    if (is_method_call) out << "_m_func"; else emit_node(node.as.call_expr.target);
                    out << ", _dyn_args.data(), _dyn_args.size());\n";
                    out << "    L->shadow_top -= _dyn_args.size();\n";
                }

                if (is_method_call) out << "    L->shadow_top -= 2;\n";

                if (want_multi) out << "    return _main_ret;\n";
                else out << "    return (_main_ret.count > 0) ? _main_ret[0] : clx::LValue();\n";
            } else {
                if (node.as.call_expr.arg_count > 0) {
                    out << "    clx::LValue args[] = {";
                    for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i) {
                        if (is_method_call && i == 0) out << "_m_self";
                        else emit_node(ctx.block_statements[node.as.call_expr.first_arg + i]);
                        if (i < node.as.call_expr.arg_count - 1) out << ", ";
                    }
                    out << "};\n";
                    if (is_direct) {
                        out << "    clx::MultiValue _main_ret = _impl_" << fname << "(L, args, " << node.as.call_expr.arg_count << ");\n";
                    } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size() && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
                        auto _chit = CodeEmitter::g_hoisted_lookups.find(node.as.call_expr.target);
                        if (_chit != CodeEmitter::g_hoisted_lookups.end()) {
                            auto _cf_it = CodeEmitter::g_hoisted_cfuncs.find(_chit->second);
                            if (_cf_it != CodeEmitter::g_hoisted_cfuncs.end()) {
                                out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count << "; ++i) L->shadow_stack[L->shadow_top++] = &args[i];\n";
                                out << "    clx::MultiValue _main_ret = clx::" << _cf_it->second << "(L, args, " << node.as.call_expr.arg_count << ");\n";
                                out << "    L->shadow_top -= " << node.as.call_expr.arg_count << ";\n";
                            } else { goto _call_direct_normal; }
                        } else { goto _call_direct_normal; }
                    } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size() && ctx.nodes[node.as.call_expr.target].type == NodeType::Identifier && !ctx.nodes[node.as.call_expr.target].as.ident.is_global) {
                        std::string_view _alias_nm(ctx.nodes[node.as.call_expr.target].as.ident.name, ctx.nodes[node.as.call_expr.target].as.ident.length);
                        auto _alias_it = CodeEmitter::g_builtin_aliases.find(std::string(_alias_nm));
                        if (_alias_it != CodeEmitter::g_builtin_aliases.end() && CodeEmitter::g_reassigned_vars.count(std::string(_alias_nm)) == 0) {
                            out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count << "; ++i) L->shadow_stack[L->shadow_top++] = &args[i];\n";
                            out << "    clx::MultiValue _main_ret = clx::" << _alias_it->second << "(L, args, " << node.as.call_expr.arg_count << ");\n";
                            out << "    L->shadow_top -= " << node.as.call_expr.arg_count << ";\n";
                        } else { goto _call_direct_normal; }
                    } else {
                        _call_direct_normal:
                        out << "    for (size_t i = 0; i < " << node.as.call_expr.arg_count << "; ++i) L->shadow_stack[L->shadow_top++] = &args[i];\n";
                        out << "    clx::MultiValue _main_ret = clx::call_direct(L, ";
                        if (is_method_call) out << "_m_func"; else emit_node(node.as.call_expr.target);
                        out << ", args, " << node.as.call_expr.arg_count << ");\n";
                        out << "    L->shadow_top -= " << node.as.call_expr.arg_count << ";\n";
                    }
                } else {
                    if (is_direct) {
                        out << "    clx::MultiValue _main_ret = _impl_" << fname << "(L, nullptr, 0);\n";
                    } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size() && ctx.nodes[node.as.call_expr.target].type == NodeType::TableAccess) {
                        auto _chit = CodeEmitter::g_hoisted_lookups.find(node.as.call_expr.target);
                        if (_chit != CodeEmitter::g_hoisted_lookups.end()) {
                            auto _cf_it = CodeEmitter::g_hoisted_cfuncs.find(_chit->second);
                            if (_cf_it != CodeEmitter::g_hoisted_cfuncs.end())
                                out << "    clx::MultiValue _main_ret = clx::" << _cf_it->second << "(L, nullptr, 0);\n";
                            else { goto _call_direct_normal2; }
                        } else { goto _call_direct_normal2; }
                    } else if (!is_method_call && node.as.call_expr.target < ctx.nodes.size() && ctx.nodes[node.as.call_expr.target].type == NodeType::Identifier && !ctx.nodes[node.as.call_expr.target].as.ident.is_global) {
                        std::string_view _alias_nm(ctx.nodes[node.as.call_expr.target].as.ident.name, ctx.nodes[node.as.call_expr.target].as.ident.length);
                        auto _alias_it = CodeEmitter::g_builtin_aliases.find(std::string(_alias_nm));
                        if (_alias_it != CodeEmitter::g_builtin_aliases.end() && CodeEmitter::g_reassigned_vars.count(std::string(_alias_nm)) == 0)
                            out << "    clx::MultiValue _main_ret = clx::" << _alias_it->second << "(L, nullptr, 0);\n";
                        else { goto _call_direct_normal2; }
                    } else { _call_direct_normal2:;
                        out << "    clx::MultiValue _main_ret = clx::call_direct(L, ";
                        if (is_method_call) out << "_m_func"; else emit_node(node.as.call_expr.target);
                        out << ", nullptr, 0);\n";
                    }
                }

                if (is_method_call) out << "    L->shadow_top -= 2;\n";

                if (want_multi) out << "    return _main_ret;\n";
                else out << "    return (_main_ret.count > 0) ? _main_ret[0] : clx::LValue();\n";
            }
            out << "}())";
            break;
        }

        //------------------ NodeType::ParenExpression: emits a parenthesized expression
        case NodeType::ParenExpression: {
            bool want_multi = CodeEmitter::g_expect_multivalue;
            CodeEmitter::g_expect_multivalue = false;

            if (want_multi) {
                out << "clx::MultiValue(";
                emit_node(node.as.paren_expr.expr);
                out << ")";
            } else {
                emit_node(node.as.paren_expr.expr);
            }
            CodeEmitter::g_expect_multivalue = want_multi;
            break;
        }

        //------------------ NodeType::LabelStatement: emits a label target
        case NodeType::LabelStatement: {
            uint32_t name_idx = node.as.label_stmt.name_ident;
            std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);

            out << "clx_lbl_" << lname << "_" << node_idx << ":;\n";
            break;
        }

        //------------------ NodeType::GotoStatement: emits a goto statement
        case NodeType::GotoStatement: {
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            uint32_t name_idx = node.as.goto_stmt.name_ident;
            std::string_view lname(ctx.nodes[name_idx].as.ident.name, ctx.nodes[name_idx].as.ident.length);

            uint32_t target_label = CodeEmitter::g_goto_targets[node_idx];
            out << "goto clx_lbl_" << lname << "_" << target_label << ";\n";
            break;
        }

        //------------------ NodeType::Block: emits a block of statements
        case NodeType::Block: {
            bool prev_skip_braces = CodeEmitter::g_skip_block_braces;
            CodeEmitter::g_skip_block_braces = false;
            if (!prev_skip_braces) out << "{\n";
            size_t prev_native_count = CodeEmitter::g_native_numbers.size();
            bool needs_guard = false;
            for (uint32_t i = 0; i < node.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
                const auto& stmt = ctx.nodes[stmt_idx];
                if (stmt.type == NodeType::LocalDecl) {
                    for(uint32_t j=0; j<stmt.as.local_decl.ident_count; ++j) {
                        uint32_t id_idx = ctx.block_statements[stmt.as.local_decl.first_ident + j];
                        std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                        bool is_n = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
                        bool is_cap = ctx.nodes[id_idx].as.ident.is_captured;
                        if (!is_n || is_cap) needs_guard = true;
                    }
                }
            }
            if (needs_guard) out << "clx::ScopeGuard _sg_block_" << node_idx << "(L);\n";
            auto prev_hoisted = CodeEmitter::g_hoisted_locals;
            CodeEmitter::g_hoisted_locals.clear();

            bool has_goto = false;
            for (uint32_t i = 0; i < node.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
                auto t = ctx.nodes[stmt_idx].type;
                if (t == NodeType::GotoStatement) { has_goto = true; break; }
            }
            if (has_goto) {
                bool after_goto = false;
                for (uint32_t i = 0; i < node.as.block.count; ++i) {
                    uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
                    const auto& st = ctx.nodes[stmt_idx];
                    if (st.type == NodeType::GotoStatement) after_goto = true;
                    if (st.type == NodeType::LabelStatement) after_goto = false;
                    if (after_goto && st.type == NodeType::LocalDecl) {
                        bool hoist_last_is_call = false;
                        if (st.as.local_decl.value_count > 0) {
                            uint32_t last_v = ctx.block_statements[st.as.local_decl.first_value + st.as.local_decl.value_count - 1];
                            if (ctx.nodes[last_v].type == NodeType::CallExpression || ctx.nodes[last_v].type == NodeType::Vararg)
                                hoist_last_is_call = true;
                        }
                        for (uint32_t j = 0; j < st.as.local_decl.ident_count; ++j) {
                            uint32_t id_idx = ctx.block_statements[st.as.local_decl.first_ident + j];
                            std::string_view nm(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                            if (CodeEmitter::g_hoisted_locals.count(nm)) continue;
                            bool is_cap = ctx.nodes[id_idx].as.ident.is_captured;
                            bool in_native = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), nm) != CodeEmitter::g_native_numbers.end();
                            if (is_cap) {
                                out << "clx::LUpValue l_" << nm << ";\n";
                                out << "l_" << nm << " = clx::make_upvalue(clx::LValue());\n";
                                out << "L->shadow_stack[L->shadow_top++] = l_" << nm << ".get();\n";
                            } else if (in_native) {
                                out << "double l_" << nm << ";\n";
                            } else {
                                out << "clx::LValue l_" << nm << ";\n";
                                out << "L->shadow_stack[L->shadow_top++] = &l_" << nm << ";\n";
                            }
                            CodeEmitter::g_hoisted_locals.insert(nm);
                        }
                        if (hoist_last_is_call) {
                            out << "clx::MultiValue _mret_" << stmt_idx << ";\n";
                        }
                    }
                }
            }




            for (uint32_t _pi = 0; _pi < node.as.block.count; ++_pi) {
                uint32_t _ps = ctx.block_statements[node.as.block.first_statement + _pi];
                const auto& _pst = ctx.nodes[_ps];
                if (_pst.type != NodeType::LocalDecl && _pst.type != NodeType::GlobalDeclStatement && _pst.type != NodeType::Assignment) continue;
                uint32_t _ptc = _pst.type == NodeType::LocalDecl ? _pst.as.local_decl.ident_count : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.ident_count : _pst.as.assign.target_count);
                uint32_t _pvc = _pst.type == NodeType::LocalDecl ? _pst.as.local_decl.value_count : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.value_count : _pst.as.assign.value_count);
                uint32_t _pfv = _pst.type == NodeType::LocalDecl ? _pst.as.local_decl.first_value : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.first_value : _pst.as.assign.first_value);
                uint32_t _pft = _pst.type == NodeType::LocalDecl ? _pst.as.local_decl.first_ident : (_pst.type == NodeType::GlobalDeclStatement ? _pst.as.global_decl.first_ident : _pst.as.assign.first_target);
                for (uint32_t _pj = 0; _pj < _pvc && _pj < _ptc; ++_pj) {
                    uint32_t _pvi = ctx.block_statements[_pfv + _pj];
                    if (ctx.nodes[_pvi].type != NodeType::FunctionDef) continue;
                    uint32_t _pti = ctx.block_statements[_pft + _pj];
                    if (ctx.nodes[_pti].type != NodeType::Identifier || ctx.nodes[_pti].as.ident.is_global) continue;
                    std::string_view _pn(ctx.nodes[_pti].as.ident.name, ctx.nodes[_pti].as.ident.length);
                    if (CodeEmitter::g_reassigned_vars.count(_pn)) continue;
                    bool _pf = false;
                    if (CodeEmitter::g_native_return_funcs.count(_pn) && CodeEmitter::g_func_param_native.count(_pn)) {
                        _pf = true;
                        for (bool _pp : CodeEmitter::g_func_param_native[_pn]) if (!_pp) { _pf = false; break; }
                    }
                    if (_pf) CodeEmitter::g_fast_callables.insert(_pn);
                    CodeEmitter::g_direct_callables.insert(_pn);
                }
            }

            size_t prev_locals = locals.size();
            int redecl_scopes = 0;
            std::set<std::string_view> current_block_vars;

            for (uint32_t i = 0; i < node.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
                const auto& stmt = ctx.nodes[stmt_idx];

                if (stmt.type == NodeType::LocalDecl) {
                    bool creates_shadow = false;
                    for (uint32_t j = 0; j < stmt.as.local_decl.ident_count; ++j) {
                        uint32_t id_idx = ctx.block_statements[stmt.as.local_decl.first_ident + j];
                        std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                        if (current_block_vars.count(name)) creates_shadow = true;
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
                if (stmt.type != NodeType::Block &&
                    stmt.type != NodeType::Assignment &&
                    stmt.type != NodeType::GlobalDeclStatement &&
                    stmt.type != NodeType::LocalDecl &&
                    stmt.type != NodeType::BreakStatement &&
                    stmt.type != NodeType::ReturnStatement &&
                    stmt.type != NodeType::IfStatement &&
                    stmt.type != NodeType::WhileStatement &&
                    stmt.type != NodeType::RepeatStatement &&
                    stmt.type != NodeType::ForStatement &&
                    stmt.type != NodeType::GenericForStatement &&
                    stmt.type != NodeType::GotoStatement &&
                    stmt.type != NodeType::LabelStatement &&
                    stmt.type != NodeType::DoStatement) {
                    out << ";\n";
                }
            }

            for (int i = 0; i < redecl_scopes; ++i) {
                out << "}\n";
            }

            locals.resize(prev_locals);
            CodeEmitter::g_native_numbers.resize(prev_native_count);
            CodeEmitter::g_hoisted_locals = prev_hoisted;
            CodeEmitter::g_skip_block_braces = prev_skip_braces;
            if (!prev_skip_braces) out << "}\n";
            break;
        }

        //------------------ NodeType::FunctionDef: emits a function definition
        case NodeType::FunctionDef: {
            bool is_raw = CodeEmitter::g_emit_raw_lambda;
            bool is_fast = CodeEmitter::g_emit_fast_lambda;
            CodeEmitter::g_emit_raw_lambda = false;
            CodeEmitter::g_emit_fast_lambda = false;
            bool prev_in_func = CodeEmitter::g_in_function_def;
            CodeEmitter::g_in_function_def = true;

            auto saved_direct_callables = CodeEmitter::g_direct_callables;
            auto saved_fast_callables = CodeEmitter::g_fast_callables;

            if (is_fast) {
                out << "[&](auto& self";
                for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
                    uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
                    std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                    out << ", double l_" << pname;
                }
                out << ") -> double {\n";

                size_t prev_locals = locals.size();
                for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
                    uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
                    std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                    locals.push_back({pname, false});
                }

                if (node.as.func_def.body_block != 0xFFFFFFFF) emit_node(node.as.func_def.body_block);

                locals.resize(prev_locals);
                CodeEmitter::g_in_function_def = false;
                CodeEmitter::g_direct_callables = std::move(saved_direct_callables);
                CodeEmitter::g_fast_callables = std::move(saved_fast_callables);
                out << "return 0.0;\n}";
                break;
            }

            if (!is_raw) out << "L->create_closure(";
            if (!CodeEmitter::g_ref_capture.empty()) {
                out << "[=, &" << CodeEmitter::g_ref_capture << "]";
            } else {
                out << "[=]";
            }
            out << "(clx::LState* L, const clx::LValue* args, size_t arg_count) mutable -> clx::MultiValue {\n";
            out << "clx::ScopeGuard _sg_func(L);\n";
            out << "clx::CacheSlot __cs[" << CodeEmitter::g_cs_max << "]{};\n";
            int prev_cs_index = CodeEmitter::g_cs_index;
            CodeEmitter::g_cs_index = 0;
            out << "size_t _va_count = (arg_count > " << node.as.func_def.param_count << ") ? (arg_count - " << node.as.func_def.param_count << ") : 0;\n";
            out << "const clx::LValue* _va_args = _va_count > 0 ? (args + " << node.as.func_def.param_count << ") : nullptr;\n";

            for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
                uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
                std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
                bool is_native = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), pname) != CodeEmitter::g_native_numbers.end();

                if (is_cap) {
                    if (CodeEmitter::g_constant_upvalues.count(pname)) {
                        out << "clx::LValue l_" << pname << " = (" << i << " < arg_count) ? args[" << i << "] : clx::LValue();\n";
                        out << "L->shadow_stack[L->shadow_top++] = &l_" << pname << ";\n";
                    } else {
                        out << "clx::LUpValue l_" << pname << ";\n";
                        out << "l_" << pname << " = clx::make_upvalue((" << i << " < arg_count) ? args[" << i << "] : clx::LValue());\n";
                        out << "L->shadow_stack[L->shadow_top++] = l_" << pname << ".get();\n";
                    }
                } else if (is_native) {
                    out << "double l_" << pname << " = (" << i << " < arg_count) ? args[" << i << "].as_number() : 0.0;\n";
                } else {
                    out << "clx::LValue l_" << pname << " = (" << i << " < arg_count) ? args[" << i << "] : clx::LValue();\n";
                    out << "L->shadow_stack[L->shadow_top++] = &l_" << pname << ";\n";
                }
                if (CodeEmitter::g_string_builders.count(pname) && !CodeEmitter::g_global_string_builders.count(pname) && !CodeEmitter::g_module_string_builders.count(pname)) {
                    out << "clx::StringBuilder sb_" << pname << ";\n";
                }
            }

            if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
                std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name, ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
                out << "clx::LValue l_" << vaname << " = L->create_table(_va_count, 1);\n";
                out << "clx::LTable* _t_" << vaname << " = static_cast<clx::LTable*>(l_" << vaname << ".as_pointer());\n";
                out << "_t_" << vaname << "->settable(clx::LValue(L->intern_string(\"n\")), clx::LValue(static_cast<double>(_va_count)));\n";
                out << "for (size_t i = 0; i < _va_count; ++i) {\n";
                out << "    _t_" << vaname << "->settable(clx::LValue(static_cast<double>(i + 1)), _va_args[i]);\n";
                out << "}\n";
                out << "L->shadow_stack[L->shadow_top++] = &l_" << vaname << ";\n";
            }

            size_t prev_locals = locals.size();
            for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
                uint32_t p_idx = ctx.block_statements[node.as.func_def.first_param + i];
                std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
                bool is_cap = ctx.nodes[p_idx].as.ident.is_captured;
                bool param_is_boxed = is_cap && !CodeEmitter::g_constant_upvalues.count(pname);
                locals.push_back({pname, param_is_boxed});
            }
            if (node.as.func_def.is_vararg && node.as.func_def.named_vararg_ident != 0xFFFFFFFF) {
                std::string_view vaname(ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.name, ctx.nodes[node.as.func_def.named_vararg_ident].as.ident.length);
                locals.push_back({vaname, false});
            }

            if (node.as.func_def.body_block != 0xFFFFFFFF) {
                emit_node(node.as.func_def.body_block);
            }

            locals.resize(prev_locals);
            CodeEmitter::g_in_function_def = prev_in_func;
            CodeEmitter::g_direct_callables = std::move(saved_direct_callables);
            CodeEmitter::g_fast_callables = std::move(saved_fast_callables);
            CodeEmitter::g_cs_index = prev_cs_index;
            out << "return clx::MultiValue();\n";
            out << "}";
            if (!is_raw) out << ")";
            break;
        }

        //------------------ NodeType::ReturnStatement: emits a return statement
        case NodeType::ReturnStatement: {
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            uint32_t v_count = node.as.return_stmt.value_count;
            uint32_t first_v = node.as.return_stmt.first_value;

            if (CodeEmitter::g_in_fast_function) {
                if (v_count == 0) {
                    out << "return 0.0;\n";
                } else {
                    out << "return ";
                    emit_native(emit_native, ctx.block_statements[first_v]);
                    out << ";\n";
                }
                break;
            }

            if (v_count == 0) {
                if (CodeEmitter::g_in_function_def) {
                    out << "return clx::MultiValue();\n";
                } else {
                    out << "return clx::LValue();\n";
                }
                break;
            }

            bool last_is_call = false;
            uint32_t last_v_idx = ctx.block_statements[first_v + v_count - 1];
            if (ctx.nodes[last_v_idx].type == NodeType::CallExpression) {
                last_is_call = true;
            }

            if (v_count == 1 && last_is_call) {
                const auto& call_node = ctx.nodes[last_v_idx];
                bool is_direct = false;
                std::string_view fname;
                uint32_t tgt = call_node.as.call_expr.target;
                if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                    fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                    if (CodeEmitter::g_direct_callables.count(fname)) is_direct = true;
                }

                out << "{\n";
                bool last_expands = false;
                uint32_t last_arg = 0xFFFFFFFF;
                if (call_node.as.call_expr.arg_count > 0) {
                    last_arg = ctx.block_statements[call_node.as.call_expr.first_arg + call_node.as.call_expr.arg_count - 1];
                    if (ctx.nodes[last_arg].type == NodeType::CallExpression || ctx.nodes[last_arg].type == NodeType::Vararg) last_expands = true;
                }

                if (last_expands) {
                    out << "    std::vector<clx::LValue> _dyn_args;\n";
                    for (uint32_t a = 0; a < call_node.as.call_expr.arg_count - 1; ++a) {
                        out << "    _dyn_args.push_back(";
                        emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                        out << ");\n";
                    }
                    CodeEmitter::g_expect_multivalue = true;
                    out << "    clx::MultiValue _mret = ";
                    emit_node(last_arg);
                    out << ";\n";
                    CodeEmitter::g_expect_multivalue = false;
                    out << "    for (size_t _mi = 0; _mi < _mret.count; ++_mi) _dyn_args.push_back(_mret[_mi]);\n";

                    if (!is_direct) out << "    for (size_t i = 0; i < _dyn_args.size(); ++i) L->shadow_stack[L->shadow_top++] = &_dyn_args[i];\n";
                    if (is_direct) {
                        if (CodeEmitter::g_in_function_def) {
                            out << "    CLX_MUSTTAIL return _impl_" << fname << "(L, _dyn_args.data(), _dyn_args.size());\n";
                        } else {
                            out << "    clx::MultiValue _res = _impl_" << fname << "(L, _dyn_args.data(), _dyn_args.size());\n";
                            out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                        }
                    } else {
                        if (CodeEmitter::g_in_function_def) {
                            out << "    CLX_MUSTTAIL return clx::call_function(L, ";
                            emit_node(tgt);
                            out << ", _dyn_args.data(), _dyn_args.size(), \"" << ctx.filename << "\", " << call_node.line << ");\n";
                        } else {
                            out << "    clx::MultiValue _res = clx::call_function(L, ";
                            emit_node(tgt);
                            out << ", _dyn_args.data(), _dyn_args.size(), \"" << ctx.filename << "\", " << call_node.line << ");\n";
                            out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                        }
                    }
                } else {
                    if (call_node.as.call_expr.arg_count > 0) {
                        out << "    clx::LValue args_" << last_v_idx << "[] = {";
                        for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                            emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                            if (a < call_node.as.call_expr.arg_count - 1) out << ", ";
                        }
                        out << "};\n";
                        if (!is_direct) out << "    for (size_t i = 0; i < " << call_node.as.call_expr.arg_count << "; ++i) L->shadow_stack[L->shadow_top++] = &args_" << last_v_idx << "[i];\n";

                        if (is_direct) {
                            if (CodeEmitter::g_in_function_def) {
                                out << "    CLX_MUSTTAIL return _impl_" << fname << "(L, args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                            } else {
                                out << "    clx::MultiValue _res = _impl_" << fname << "(L, args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                                out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                            }
                        } else {
                            if (CodeEmitter::g_in_function_def) {
                                out << "    CLX_MUSTTAIL return clx::call_function(L, ";
                                emit_node(tgt);
                                out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", \"" << ctx.filename << "\", " << call_node.line << ");\n";
                            } else {
                                out << "    clx::MultiValue _res = clx::call_function(L, ";
                                emit_node(tgt);
                                out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", \"" << ctx.filename << "\", " << call_node.line << ");\n";
                                out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                            }
                        }
                    } else {
                        if (is_direct) {
                            if (CodeEmitter::g_in_function_def) {
                                out << "    CLX_MUSTTAIL return _impl_" << fname << "(L, nullptr, 0);\n";
                            } else {
                                out << "    clx::MultiValue _res = _impl_" << fname << "(L, nullptr, 0);\n";
                                out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                            }
                        } else {
                            if (CodeEmitter::g_in_function_def) {
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
                break;
            }

            bool has_vararg = false;
            for (size_t i = 0; i < v_count; ++i) {
                if (ctx.nodes[ctx.block_statements[first_v + i]].type == NodeType::Vararg) has_vararg = true;
            }

            out << "{\n";
            if (!last_is_call && !has_vararg) {
                out << "    clx::LValue _ret_args[" << std::max(1u, v_count) << "];\n";
                for (size_t i = 0; i < v_count; ++i) {
                    out << "    _ret_args[" << i << "] = ";
                    emit_node(ctx.block_statements[first_v + i]);
                    out << ";\n";
                }
                if (CodeEmitter::g_in_function_def) {
                    out << "    return clx::MultiValue(_ret_args, " << v_count << ");\n";
                } else {
                    out << "    return _ret_args[0];\n";
                }
            } else {
                out << "    std::vector<clx::LValue> _ret_vals;\n";
                for (size_t i = 0; i < v_count; ++i) {
                    uint32_t v_idx = ctx.block_statements[first_v + i];
                    if (i == v_count - 1 && (last_is_call || ctx.nodes[v_idx].type == NodeType::Vararg)) {
                        CodeEmitter::g_expect_multivalue = true;
                        out << "    clx::MultiValue _mret = ";
                        emit_node(v_idx);
                        out << ";\n";
                        CodeEmitter::g_expect_multivalue = false;
                        out << "    for (size_t m = 0; m < _mret.count; ++m) _ret_vals.push_back(_mret[m]);\n";
                    } else {
                        out << "    clx::LValue _tmp_" << node_idx << "_" << i << " = ";
                        emit_node(v_idx);
                        out << ";\n";
                        out << "    _ret_vals.push_back(_tmp_" << node_idx << "_" << i << ");\n";
                    }
                }
                if (CodeEmitter::g_in_function_def) {
                    out << "    return clx::MultiValue(_ret_vals);\n";
                } else {
                    out << "    return (!_ret_vals.empty()) ? _ret_vals[0] : clx::LValue();\n";
                }
            }
            out << "}\n";
            break;
        }

        //------------------ NodeType::GlobalDeclStatement: emits a global declaration
        case NodeType::GlobalDeclStatement:
        //------------------ NodeType::LocalDecl: emits a local declaration
        case NodeType::LocalDecl:
        //------------------ NodeType::Assignment: emits an assignment statement
        case NodeType::Assignment: {
            if (node.type == NodeType::GlobalDeclStatement && node.as.global_decl.is_wildcard) break;
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";

            bool is_local = (node.type == NodeType::LocalDecl);
            bool is_global = (node.type == NodeType::GlobalDeclStatement);
            uint32_t t_count = is_local ? node.as.local_decl.ident_count : (is_global ? node.as.global_decl.ident_count : node.as.assign.target_count);
            uint32_t v_count = is_local ? node.as.local_decl.value_count : (is_global ? node.as.global_decl.value_count : node.as.assign.value_count);
            uint32_t first_t = is_local ? node.as.local_decl.first_ident : (is_global ? node.as.global_decl.first_ident : node.as.assign.first_target);
            uint32_t first_v = is_local ? node.as.local_decl.first_value : (is_global ? node.as.global_decl.first_value : node.as.assign.first_value);

            bool last_is_call = false;
            if (v_count > 0) {
                uint32_t last_v_idx = ctx.block_statements[first_v + v_count - 1];
                if (ctx.nodes[last_v_idx].type == NodeType::CallExpression || ctx.nodes[last_v_idx].type == NodeType::Vararg) last_is_call = true;
            }

            bool is_single_native = false;
            std::string_view single_name;
            if (t_count == 1 && !last_is_call) {
                uint32_t t_idx = ctx.block_statements[first_t];
                if (ctx.nodes[t_idx].type == NodeType::Identifier && !ctx.nodes[t_idx].as.ident.is_global) {
                    single_name = std::string_view(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    bool in_native = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), single_name) != CodeEmitter::g_native_numbers.end();
                    bool rhs_yields_number = yields_number(ctx, ctx.block_statements[first_v], nullptr, CodeEmitter::g_current_fast_func);
                    if (!ctx.nodes[t_idx].as.ident.is_captured) {
                        if (is_local) {
                            if (in_native || rhs_yields_number) is_single_native = true;
                        } else {
                            if (in_native && rhs_yields_number) is_single_native = true;
                        }
                    }
                }
            }

            if (is_single_native) {


                if (std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), single_name) == CodeEmitter::g_native_numbers.end())
                    CodeEmitter::g_native_numbers.push_back(single_name);
                if (is_local) {
                    if (!CodeEmitter::g_hoisted_locals.count(single_name))
                        out << "double l_" << single_name << ";\n";
                    out << "l_" << single_name << " = ";
                    if (v_count > 0) {
                        uint32_t v_idx = ctx.block_statements[first_v];
                        emit_native(emit_native, v_idx);
                    } else out << "0.0";
                    out << ";\n";
                    locals.push_back({single_name, false});
                } else {
                    bool is_boxed = false;
                    bool is_loc = this->is_local(single_name, is_boxed);
                    if (is_boxed) {
                        out << "(*l_" << single_name << ") = clx::LValue(static_cast<double>(";
                        if (v_count > 0) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            emit_native(emit_native, v_idx);
                        } else out << "0.0";
                        out << "))";
                    } else {
                        out << "l_" << single_name << " = ";
                        if (v_count > 0) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            emit_native(emit_native, v_idx);
                        } else out << "0.0";
                    }
                    out << ";\n";
                }
                break;
            }

            bool is_single_dynamic = (t_count == 1 && v_count <= 1 && !last_is_call && !is_single_native);
            if (is_single_dynamic) {
                uint32_t t_idx = ctx.block_statements[first_t];
                const auto& t_node = ctx.nodes[t_idx];

                if (t_node.type == NodeType::Identifier) {
                    std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
                    bool is_boxed = false;
                    bool is_loc = this->is_local(name, is_boxed);

                    if (is_local) {
                        bool intercepted = false;
                        if (v_count > 0) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            if (ctx.nodes[v_idx].type == NodeType::FunctionDef && !t_node.as.ident.is_global) {
                                if (CodeEmitter::g_reassigned_vars.count(name) == 0) {
                                    bool is_fast = false;
                                    if (CodeEmitter::g_native_return_funcs.count(name) && CodeEmitter::g_func_param_native.count(name)) {
                                        is_fast = true;
                                        for (bool p : CodeEmitter::g_func_param_native[name]) if (!p) is_fast = false;
                                    }

                                    if (is_fast) {
                                        out << "auto _fast_" << name << "_impl = ";
                                        CodeEmitter::g_emit_fast_lambda = true;
                                        CodeEmitter::g_in_fast_function = true;
                                        CodeEmitter::g_current_fast_func = name;
                                        emit_node(v_idx);
                                        CodeEmitter::g_emit_fast_lambda = false;
                                        CodeEmitter::g_in_fast_function = false;
                                        CodeEmitter::g_current_fast_func = "";
                                        out << ";\n";

                                        out << "auto _fast_" << name << " = [&]( ";
                                        for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[name]; ++a) {
                                            out << "double p" << a << (a < CodeEmitter::g_func_param_counts[name] - 1 ? ", " : "");
                                        }
                                        out << ") -> double { return _fast_" << name << "_impl(_fast_" << name << "_impl";
                                        for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[name]; ++a) {
                                            out << ", p" << a;
                                        }
                                        out << "); };\n";

                                        out << "auto _impl_" << name << " = [=](clx::LState* L, const clx::LValue* args, size_t arg_count) -> clx::MultiValue {\n";
                                        out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << name << "(";
                                        for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[name]; ++a) {
                                            out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                            if (a < CodeEmitter::g_func_param_counts[name] - 1) out << ", ";
                                        }
                                        out << "))));\n};\n";
                                        out << "clx::LValue l_" << name << " = L->create_closure(_impl_" << name << ");\n";
                                        out << "L->shadow_stack[L->shadow_top++] = &l_" << name << ";\n";
                                        { size_t _sfi = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name)); out << "L->_G->settable(cstr_[" << _sfi << "], l_" << name << ");\n"; }
                                        locals.push_back({name, false});
                                        CodeEmitter::g_fast_callables.insert(name);
                                    } else {
                                        out << "clx::LValue l_" << name << ";\n";
                                        CodeEmitter::g_ref_capture = std::string("l_") + std::string(name) + ", &_impl_" + std::string(name);
                                        if (CodeEmitter::g_string_builders.count(name) && !CodeEmitter::g_module_string_builders.count(name)) {
                                            CodeEmitter::g_ref_capture += ", sb_" + std::string(name);
                                        }
                                        locals.push_back({name, false});
                                        CodeEmitter::g_direct_callables.insert(name);
                                        out << "std::function<clx::MultiValue(clx::LState*, const clx::LValue*, size_t)> _impl_" << name << ";\n";
                                        out << "_impl_" << name << " = ";
                                        CodeEmitter::g_emit_raw_lambda = true;
                                        emit_node(v_idx);
                                        CodeEmitter::g_emit_raw_lambda = false;
                                        CodeEmitter::g_ref_capture.clear();
                                        out << ";\n";
                                        out << "l_" << name << " = L->create_closure(_impl_" << name << ");\n";
                                        out << "L->shadow_stack[L->shadow_top++] = &l_" << name << ";\n";
                                        { size_t _sfi = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name)); out << "L->_G->settable(cstr_[" << _sfi << "], l_" << name << ");\n"; }
                                    }

                                    intercepted = true;
                                }
                            }
                        }

                        if (!intercepted) {
                            if (v_count > 0 && ctx.nodes[ctx.block_statements[first_v]].type == NodeType::TableConstructor && CodeEmitter::g_pure_numeric_arrays.count(name)) {
                                uint32_t v_idx = ctx.block_statements[first_v];
                                if (CodeEmitter::g_table_presize.count(v_idx)) {
                                    out << "std::vector<double> l_" << name << "(static_cast<size_t>(";
                                    emit_native(emit_native, CodeEmitter::g_table_presize[v_idx]);
                                    out << "), 0.0);\n";
                                } else {
                                    out << "std::vector<double> l_" << name << ";\n";
                                }
                                locals.push_back({name, false});
                                } else if (t_node.as.ident.is_captured) {
                                bool is_const = CodeEmitter::g_constant_upvalues.count(name) > 0;
                                if (is_const) {
                                    {
                                        uint32_t _alias_v_idx3 = ctx.block_statements[first_v];
                                        const auto& _alias_v_node3 = ctx.nodes[_alias_v_idx3];
                                        if (v_count > 0 && !t_node.as.ident.is_global &&
                                            _alias_v_node3.type == NodeType::TableAccess &&
                                            CodeEmitter::g_reassigned_vars.count(name) == 0) {
                                            uint32_t _ht4 = _alias_v_node3.as.table_access.table;
                                            uint32_t _hk4 = _alias_v_node3.as.table_access.key;
                                            if (_ht4 < ctx.nodes.size() && _hk4 < ctx.nodes.size() &&
                                                ctx.nodes[_ht4].type == NodeType::Identifier && ctx.nodes[_hk4].type == NodeType::String) {
                                                std::string_view _hm4(ctx.nodes[_ht4].as.ident.name, ctx.nodes[_ht4].as.ident.length);
                                                std::string_view _hf4(ctx.nodes[_hk4].as.string.text, ctx.nodes[_hk4].as.string.length);
                                                const char* _cf4 = lookup_builtin(_hm4, _hf4);
                                                if (_cf4)
                                                    CodeEmitter::g_builtin_aliases[std::string(name)] = _cf4;
                                            }
                                        }
                                    }
                                    if (CodeEmitter::g_hoisted_locals.count(name)) {
                                        if (v_count > 0) {
                                            out << "l_" << name << " = ";
                                            emit_node(ctx.block_statements[first_v]);
                                            out << ";\n";
                                        }
                                    } else {
                                    out << "clx::LValue l_" << name << " = ";
                                    if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                                    else out << "clx::LValue()";
                                    out << ";\nL->shadow_stack[L->shadow_top++] = &l_" << name << ";\n";
                                    }
                                    if (CodeEmitter::g_string_builders.count(name) && !CodeEmitter::g_global_string_builders.count(name) && !CodeEmitter::g_module_string_builders.count(name)) {
                                        out << "clx::StringBuilder sb_" << name << ";\n";
                                    }
                                    locals.push_back({name, false});

                                    if (t_node.as.ident.attr == clx::Attribute::Close) {
                                        out << "clx::CloseGuard __cg_" << node_idx << "_0(L, l_" << name << ");\n";
                                    }
                                } else {
                                    bool is_func = v_count > 0 && ctx.nodes[ctx.block_statements[first_v]].type == NodeType::FunctionDef;
                                    if (!CodeEmitter::g_hoisted_locals.count(name)) {
                                        out << "clx::LUpValue l_" << name << ";\n";
                                    }
                                    if (is_func) {
                                        CodeEmitter::g_ref_capture = std::string("l_") + std::string(name);
                                        if (CodeEmitter::g_string_builders.count(name) && !CodeEmitter::g_module_string_builders.count(name)) {
                                            CodeEmitter::g_ref_capture += ", sb_" + std::string(name);
                                        }
                                    }
                                    locals.push_back({name, true});
                                    out << "l_" << name << " = clx::make_upvalue(";
                                    if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                                    else out << "clx::LValue()";
                                    out << ");\nL->shadow_stack[L->shadow_top++] = l_" << name << ".get();\n";
                                    if (is_func) {
                                        CodeEmitter::g_ref_capture.clear();
                                        size_t _sfi = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name));
                                        out << "L->_G->settable(cstr_[" << _sfi << "], *l_" << name << ");\n";
                                    }
                                    if (CodeEmitter::g_string_builders.count(name) && !CodeEmitter::g_global_string_builders.count(name) && !CodeEmitter::g_module_string_builders.count(name)) {
                                        out << "clx::StringBuilder sb_" << name << ";\n";
                                    }

                                    if (t_node.as.ident.attr == clx::Attribute::Close) {
                                        out << "clx::CloseGuard __cg_" << node_idx << "_0(L, *l_" << name << ");\n";
                                    }
                                }
                            } else {
                                {
                                    uint32_t _alias_v_idx = ctx.block_statements[first_v];
                                    const auto& _alias_v_node = ctx.nodes[_alias_v_idx];
                                    if (v_count > 0 && !t_node.as.ident.is_global && !t_node.as.ident.is_captured &&
                                        _alias_v_node.type == NodeType::TableAccess &&
                                        CodeEmitter::g_reassigned_vars.count(name) == 0) {
                                        uint32_t _ht2 = _alias_v_node.as.table_access.table;
                                        uint32_t _hk2 = _alias_v_node.as.table_access.key;
                                        if (_ht2 < ctx.nodes.size() && _hk2 < ctx.nodes.size() &&
                                            ctx.nodes[_ht2].type == NodeType::Identifier && ctx.nodes[_hk2].type == NodeType::String) {
                                            std::string_view _hm2(ctx.nodes[_ht2].as.ident.name, ctx.nodes[_ht2].as.ident.length);
                                            std::string_view _hf2(ctx.nodes[_hk2].as.string.text, ctx.nodes[_hk2].as.string.length);
                                            const char* _cf2 = lookup_builtin(_hm2, _hf2);
                                            if (_cf2)
                                                CodeEmitter::g_builtin_aliases[std::string(name)] = _cf2;
                                        }
                                    }
                                }
                                if (CodeEmitter::g_hoisted_locals.count(name)) {
                                    if (v_count > 0) {
                                        out << "l_" << name << " = ";
                                        emit_node(ctx.block_statements[first_v]);
                                        out << ";\n";
                                    }
                                } else {
                                out << "clx::LValue l_" << name << " = ";
                                if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                                else out << "clx::LValue()";
                                out << ";\nL->shadow_stack[L->shadow_top++] = &l_" << name << ";\n";
                                }
                                    if (CodeEmitter::g_string_builders.count(name) && !CodeEmitter::g_global_string_builders.count(name) && !CodeEmitter::g_module_string_builders.count(name)) {
                                        out << "clx::StringBuilder sb_" << name << ";\n";
                                    }
                                locals.push_back({name, false});

                                if (t_node.as.ident.attr == clx::Attribute::Close) {
                                    out << "clx::CloseGuard __cg_" << node_idx << "_0(L, l_" << name << ");\n";
                                }
                            }
                        }
                    } else if (is_global && v_count == 0) {
                    } else if (t_node.as.ident.is_global) {
                        CodeEmitter::g_global_constants.erase(name);
                        auto it = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name);
                        if (it != CodeEmitter::g_native_numbers.end()) CodeEmitter::g_native_numbers.erase(it);


                        bool is_sb_concat = false;
                        std::vector<uint32_t> ops;
                        if (v_count > 0 && CodeEmitter::g_string_builders.count(name)) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            const auto& v_node = ctx.nodes[v_idx];
                            if (v_node.type == NodeType::BinaryOp && v_node.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                                std::vector<uint32_t> ws;
                                ws.push_back(v_idx);
                                while (!ws.empty()) {
                                    uint32_t cur = ws.back(); ws.pop_back();
                                    const auto& cn = ctx.nodes[cur];
                                    if (cn.type == NodeType::BinaryOp && cn.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                                        ws.push_back(cn.as.bin_op.right);
                                        ws.push_back(cn.as.bin_op.left);
                                    } else {
                                        ops.push_back(cur);
                                    }
                                }
                                if (!ops.empty() && ctx.nodes[ops[0]].type == NodeType::Identifier) {
                                    std::string_view fn(ctx.nodes[ops[0]].as.ident.name, ctx.nodes[ops[0]].as.ident.length);
                                    if (fn == name) is_sb_concat = true;
                                }
                            }
                        }

                        if (is_sb_concat) {

                            out << "{ clx::LValue _gval = clx::get_env_var(L, _ENV, \"" << name << "\");\n";
                            out << "  if (sb_" << name << ".empty() && _gval.type() == clx::LType::String) {\n";
                            out << "    sb_" << name << ".append(_gval.as_string(), _gval.string_len()); }\n";
                            for (size_t i = 1; i < ops.size(); ++i) {
                                uint32_t op_idx = ops[i];
                                const auto& op_node = ctx.nodes[op_idx];
                                if (op_node.type == NodeType::String) {
                                    size_t sfi = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(op_node.as.string.text, op_node.as.string.length)));
                                    out << "  sb_" << name << ".append(cstr_[" << sfi << "].as_string(), cstr_[" << sfi << "].string_len());\n";
                                } else if (op_node.type == NodeType::Integer) {
                                    out << "  sb_" << name << ".append(L, clx::LValue(static_cast<double>(" << op_node.as.integer.val << ".0)));\n";
                                } else if (op_node.type == NodeType::Number) {
                                    out << "  sb_" << name << ".append(L, clx::LValue(static_cast<double>(" << op_node.as.number.val << ")));\n";
                                } else {
                                    out << "  sb_" << name << ".append(L, ";
                                    emit_node(op_idx);
                                    out << ");\n";
                                }
                            }
                            out << "  clx::set_env_var(L, _ENV, \"" << name << "\", clx::LValue(sb_" << name << ".to_string(L)));\n";
                            out << "}\n";
                        } else {
                            out << "clx::set_env_var(L, _ENV, \"" << name << "\", ";
                            if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                            else out << "clx::LValue()";
                            out << ");\n";
                        }
                    } else if (is_loc) {
                        bool intercepted = false;
                        if (v_count > 0) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            if (ctx.nodes[v_idx].type == NodeType::FunctionDef) {
                                if (CodeEmitter::g_reassigned_vars.count(name) == 0) {
                                    bool is_fast = false;
                                    if (CodeEmitter::g_native_return_funcs.count(name) && CodeEmitter::g_func_param_native.count(name)) {
                                        is_fast = true;
                                        for (bool p : CodeEmitter::g_func_param_native[name]) if (!p) is_fast = false;
                                    }

                                    if (is_fast) {
                                        out << "auto _fast_" << name << "_impl = ";
                                        CodeEmitter::g_emit_fast_lambda = true;
                                        CodeEmitter::g_in_fast_function = true;
                                        CodeEmitter::g_current_fast_func = name;
                                        emit_node(v_idx);
                                        CodeEmitter::g_emit_fast_lambda = false;
                                        CodeEmitter::g_in_fast_function = false;
                                        CodeEmitter::g_current_fast_func = "";
                                        out << ";\n";

                                        out << "auto _fast_" << name << " = [&]( ";
                                        for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[name]; ++a) {
                                            out << "double p" << a << (a < CodeEmitter::g_func_param_counts[name] - 1 ? ", " : "");
                                        }
                                        out << ") -> double { return _fast_" << name << "_impl(_fast_" << name << "_impl";
                                        for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[name]; ++a) {
                                            out << ", p" << a;
                                        }
                                        out << "); };\n";

                                        out << "auto _impl_" << name << " = [=](clx::LState* L, const clx::LValue* args, size_t arg_count) -> clx::MultiValue {\n";
                                        out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << name << "(";
                                        for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[name]; ++a) {
                                            out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                            if (a < CodeEmitter::g_func_param_counts[name] - 1) out << ", ";
                                        }
                                        out << "))));\n};\n";
                                        CodeEmitter::g_fast_callables.insert(name);
                                    } else {
                                        out << "auto _impl_" << name << " = ";
                                        CodeEmitter::g_emit_raw_lambda = true;
                                        emit_node(v_idx);
                                        CodeEmitter::g_emit_raw_lambda = false;
                                        out << ";\n";
                                    }

                                    CodeEmitter::g_direct_callables.insert(name);
                                    if (is_boxed) out << "(*l_" << name << ") = L->create_closure(_impl_" << name << ");\n";
                                    else out << "l_" << name << " = L->create_closure(_impl_" << name << ");\n";
                                    { size_t _sfi2 = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name)); out << "L->_G->settable(cstr_[" << _sfi2 << "], " << (is_boxed ? "(*l_" + std::string(name) + ")" : "l_" + std::string(name)) << ");\n"; }
                                    intercepted = true;
                                }
                            }
                        }
                        if (!intercepted && v_count > 0) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            const auto& v_node = ctx.nodes[v_idx];

                            if (v_node.type == NodeType::BinaryOp && v_node.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {

                                std::vector<uint32_t> concat_ops;
                                std::vector<uint32_t> walk_stack;
                                walk_stack.push_back(v_idx);
                                while (!walk_stack.empty()) {
                                    uint32_t cur = walk_stack.back();
                                    walk_stack.pop_back();
                                    const auto& cn = ctx.nodes[cur];
                                    if (cn.type == NodeType::BinaryOp && cn.as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
                                        walk_stack.push_back(cn.as.bin_op.right);
                                        walk_stack.push_back(cn.as.bin_op.left);
                                    } else {
                                        concat_ops.push_back(cur);
                                    }
                                }


                                if (!concat_ops.empty() && ctx.nodes[concat_ops[0]].type == NodeType::Identifier) {
                                    std::string_view first_name(ctx.nodes[concat_ops[0]].as.ident.name, ctx.nodes[concat_ops[0]].as.ident.length);
                                    if (first_name == name && !ctx.nodes[concat_ops[0]].as.ident.is_global) {



                                        if (is_boxed) {
                                            out << "if (sb_" << name << ".empty()) sb_" << name << ".append((*l_" << name << ").as_string(), (*l_" << name << ").string_len());\n";
                                        } else {
                                            out << "if (sb_" << name << ".empty()) sb_" << name << ".append(l_" << name << ".as_string(), l_" << name << ".string_len());\n";
                                        }

                                        for (size_t i = 1; i < concat_ops.size(); ++i) {
                                            uint32_t op_idx = concat_ops[i];
                                            const auto& op_node = ctx.nodes[op_idx];
                                            if (op_node.type == NodeType::String) {
                                                size_t sfi = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(op_node.as.string.text, op_node.as.string.length)));
                                                out << "  sb_" << name << ".append(cstr_[" << sfi << "].as_string(), cstr_[" << sfi << "].string_len());\n";
                                            } else if (op_node.type == NodeType::Integer) {
                                                out << "sb_" << name << ".append(L, clx::LValue(static_cast<double>(" << op_node.as.integer.val << ".0)));\n";
                                            } else if (op_node.type == NodeType::Number) {
                                                out << "sb_" << name << ".append(L, clx::LValue(static_cast<double>(" << op_node.as.number.val << ")));\n";
                                            } else if (op_node.type == NodeType::CallExpression || op_node.type == NodeType::Vararg) {
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
                            if (is_boxed) out << "(*l_" << name << ") = ";
                            else out << "l_" << name << " = ";

                            if (v_count > 0) {
                                bool lhs_is_native = !is_boxed && std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
                                if (lhs_is_native) emit_native(emit_native, ctx.block_statements[first_v]);
                                else emit_node(ctx.block_statements[first_v]);
                            } else out << "clx::LValue()";
                            out << ";\n";
                        }
                    } else {
                        size_t idx = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name));
                        out << "L->_G->settable(cstr_[" << idx << "], ";
                        if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                        else out << "clx::LValue()";
                        out << ");\n";
                    }
                } else if (t_node.type == NodeType::TableAccess) {
                    std::string_view t_name;
                    if (ctx.nodes[t_node.as.table_access.table].type == NodeType::Identifier) {
                        t_name = std::string_view(ctx.nodes[t_node.as.table_access.table].as.ident.name, ctx.nodes[t_node.as.table_access.table].as.ident.length);
                    }
                    if (!t_name.empty() && CodeEmitter::g_pure_numeric_arrays.count(t_name)) {
                        out << "{ size_t _n = static_cast<size_t>(";
                        emit_native(emit_native, t_node.as.table_access.key);
                        out << "); if (_n > l_" << t_name << ".size()) l_" << t_name << ".resize(_n); l_" << t_name << "[_n - 1] = ";
                        if (v_count > 0) {
                            uint32_t v_idx = ctx.block_statements[first_v];
                            if (ctx.nodes[v_idx].type == NodeType::TrueLiteral) out << "1.0";
                            else if (ctx.nodes[v_idx].type == NodeType::FalseLiteral) out << "0.0";
                            else emit_native(emit_native, v_idx);
                        } else out << "0.0";
                        out << "; }\n";
                    } else if (CodeEmitter::g_bce_safe_nodes.count(t_idx)) {
                        out << "static_cast<clx::LTable*>(("; emit_node(t_node.as.table_access.table); out << ").as_pointer())->array[static_cast<size_t>(";
                        emit_native(emit_native, t_node.as.table_access.key);
                        out << ") - 1] = ";
                        if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                        else out << "clx::LValue()";
                        out << ";\n";
                    } else {
                        bool key_is_native = false;
                        uint32_t k_idx = t_node.as.table_access.key;
                        if (yields_number(ctx, k_idx, nullptr, CodeEmitter::g_current_fast_func)) key_is_native = true;

                        if (key_is_native) {
                            out << "{ clx::LTable* _t = static_cast<clx::LTable*>(("; emit_node(t_node.as.table_access.table); out << ").as_pointer()); size_t _k = static_cast<size_t>("; emit_native(emit_native, k_idx); out << "); if (_k - 1 < _t->array_size) _t->array[_k - 1] = ";
                            if (v_count > 0) emit_node(ctx.block_statements[first_v]); else out << "clx::LValue()";
                            out << "; else clx::table_set_int(L, clx::LValue(clx::LType::Table, _t), _k, ";
                            if (v_count > 0) emit_node(ctx.block_statements[first_v]); else out << "clx::LValue()";
                            out << "); }\n";
                        } else {
                            uint32_t kt = t_node.as.table_access.key;
                            if (kt < ctx.nodes.size() && ctx.nodes[kt].type == NodeType::String) {
                                uint32_t tbl_idx = t_node.as.table_access.table;
                                bool tbl_is_stable = tbl_idx < ctx.nodes.size() &&
                                                     ctx.nodes[tbl_idx].type == NodeType::Identifier &&
                                                     ctx.nodes[tbl_idx].as.ident.is_global;
                                int _cs_i = (tbl_is_stable && CodeEmitter::g_cs_index < CodeEmitter::g_cs_max) ? CodeEmitter::g_cs_index++ : -1;
                                if (_cs_i >= 0) {
                                    out << "clx::table_set_cs(L, "; emit_node(t_node.as.table_access.table); out << ", cstr_[" << (std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length)))) << "], ";
                                    if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                                    else out << "clx::LValue()";
                                    out << ", &__cs[" << _cs_i << "]);\n";
                                } else {
                                    out << "clx::table_set(L, "; emit_node(t_node.as.table_access.table); out << ", cstr_[" << (std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length)))) << "], ";
                                    if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                                    else out << "clx::LValue()";
                                    out << ");\n";
                                }
                            } else {
                                out << "clx::table_set(L, "; emit_node(t_node.as.table_access.table); out << ", "; emit_node(t_node.as.table_access.key); out << ", ";
                                if (v_count > 0) emit_node(ctx.block_statements[first_v]);
                                else out << "clx::LValue()";
                                out << ");\n";
                            }
                        }
                    }
                }
                break;
            }

            if (last_is_call) {
                bool _mret_hoisted = false;
                for (uint32_t _j = 0; _j < t_count; ++_j) {
                    uint32_t _t_idx = ctx.block_statements[first_t + _j];
                    std::string_view _nm(ctx.nodes[_t_idx].as.ident.name, ctx.nodes[_t_idx].as.ident.length);
                    if (CodeEmitter::g_hoisted_locals.count(_nm)) { _mret_hoisted = true; break; }
                }
                if (!_mret_hoisted)
                    out << "clx::MultiValue _mret_" << node_idx << ";\n";
            }

            std::vector<bool> tmp_is_native(v_count, false);
            if (v_count > 0) {
                for (size_t i = 0; i < v_count; ++i) {
                    uint32_t v_idx = ctx.block_statements[first_v + i];

                    bool intercepted = false;
                    if (ctx.nodes[v_idx].type == NodeType::FunctionDef && i < t_count) {
                        uint32_t t_idx = ctx.block_statements[first_t + i];
                        if (ctx.nodes[t_idx].type == NodeType::Identifier && !ctx.nodes[t_idx].as.ident.is_global) {
                            std::string_view fname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);

                            if (CodeEmitter::g_reassigned_vars.count(fname) == 0) {
                                bool is_fast = false;
                                if (CodeEmitter::g_native_return_funcs.count(fname) && CodeEmitter::g_func_param_native.count(fname)) {
                                    is_fast = true;
                                    for (bool p : CodeEmitter::g_func_param_native[fname]) if (!p) is_fast = false;
                                }

                                if (is_fast) {
                                    out << "auto _fast_" << fname << "_impl = ";
                                    CodeEmitter::g_emit_fast_lambda = true;
                                    CodeEmitter::g_in_fast_function = true;
                                    CodeEmitter::g_current_fast_func = fname;
                                    emit_node(v_idx);
                                    CodeEmitter::g_emit_fast_lambda = false;
                                    CodeEmitter::g_in_fast_function = false;
                                    CodeEmitter::g_current_fast_func = "";
                                    out << ";\n";

                                    out << "auto _fast_" << fname << " = [&]( ";
                                    for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[fname]; ++a) {
                                        out << "double p" << a << (a < CodeEmitter::g_func_param_counts[fname] - 1 ? ", " : "");
                                    }
                                    out << ") -> double { return _fast_" << fname << "_impl(_fast_" << fname << "_impl";
                                    for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[fname]; ++a) {
                                        out << ", p" << a;
                                    }
                                    out << "); };\n";

                                    out << "auto _impl_" << fname << " = [=](clx::LState* L, const clx::LValue* args, size_t arg_count) -> clx::MultiValue {\n";
                                    out << "    return clx::MultiValue(clx::LValue(static_cast<double>(_fast_" << fname << "(";
                                    for (uint32_t a = 0; a < CodeEmitter::g_func_param_counts[fname]; ++a) {
                                        out << "(" << a << " < arg_count ? args[" << a << "].as_number() : 0.0)";
                                        if (a < CodeEmitter::g_func_param_counts[fname] - 1) out << ", ";
                                    }
                                    out << "))));\n};\n";
                                    CodeEmitter::g_fast_callables.insert(fname);
                                } else {
                                    out << "auto _impl_" << fname << " = ";
                                    CodeEmitter::g_emit_raw_lambda = true;
                                    emit_node(v_idx);
                                    CodeEmitter::g_emit_raw_lambda = false;
                                    out << ";\n";
                                }

                                out << "clx::LValue _tmp_" << node_idx << "_" << i << " = L->create_closure(_impl_" << fname << ");\n";
                                CodeEmitter::g_direct_callables.insert(fname);
                                intercepted = true;
                            }
                        }
                    }

                    if (intercepted) continue;

                    if (i == v_count - 1 && last_is_call) {
                        CodeEmitter::g_expect_multivalue = true;
                        out << "_mret_" << node_idx << " = ";
                        emit_node(v_idx);
                        out << ";\n";
                        CodeEmitter::g_expect_multivalue = false;
                    } else {
                        bool t_is_n = false;
                        bool v_is_n = yields_number(ctx, v_idx, nullptr, CodeEmitter::g_current_fast_func);
                        if (i < t_count) {
                            uint32_t t_idx = ctx.block_statements[first_t + i];
                            if (ctx.nodes[t_idx].type == NodeType::Identifier) {
                                 std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                                 t_is_n = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
                            }
                        }


                        if (v_is_n) t_is_n = true;

                        if (t_is_n && v_is_n) {
                            tmp_is_native[i] = true;
                            out << "double _tmp_" << node_idx << "_" << i << " = ";
                            emit_native(emit_native, v_idx);
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
                    if (!seen_in_decl.insert(name).second) continue;
                    bool in_native = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
                    bool val_is_native = (i < v_count && tmp_is_native[i]);
                    bool is_n = in_native || val_is_native;
                    bool is_cap = ctx.nodes[t_idx].as.ident.is_captured;

                    if (is_cap) {
                        if (CodeEmitter::g_constant_upvalues.count(name)) {
                            if (!CodeEmitter::g_hoisted_locals.count(name)) {
                                out << "clx::LValue l_" << name << ";\n";
                                out << "L->shadow_stack[L->shadow_top++] = &l_" << name << ";\n";
                            }
                            locals.push_back({name, false});
                        } else {
                            if (!CodeEmitter::g_hoisted_locals.count(name)) {
                                out << "clx::LUpValue l_" << name << ";\n";
                                out << "l_" << name << " = clx::make_upvalue(clx::LValue());\n";
                                out << "L->shadow_stack[L->shadow_top++] = l_" << name << ".get();\n";
                            }
                            locals.push_back({name, true});
                        }
                    } else if (is_n) {
                        if (!in_native) CodeEmitter::g_native_numbers.push_back(name);
                        if (!CodeEmitter::g_hoisted_locals.count(name)) {
                            out << "double l_" << name << ";\n";
                            out << "l_" << name << " = 0.0;\n";
                        }
                        locals.push_back({name, false});
                    } else {
                        if (!CodeEmitter::g_hoisted_locals.count(name)) {
                            out << "clx::LValue l_" << name << ";\n";
                            out << "L->shadow_stack[L->shadow_top++] = &l_" << name << ";\n";
                        }
                        locals.push_back({name, false});
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
                            val_str = "clx::LValue(static_cast<double>(" + num_str + "))";
                        } else {
                            val_str = "_tmp_" + std::to_string(node_idx) + "_" + std::to_string(i);
                            num_str = val_str + ".as_number()";
                        }
                    }
                } else if (last_is_call) {
                    val_str = "(((" + std::to_string(i) + " - " + std::to_string(v_count - 1) + ") < _mret_" + std::to_string(node_idx) + ".count) ? _mret_" + std::to_string(node_idx) + "[" + std::to_string(i) + " - " + std::to_string(v_count - 1) + "] : clx::LValue())";
                    num_str = val_str + ".as_number()";
                } else {
                    val_str = "clx::LValue()";
                    num_str = "0.0";
                }

                uint32_t t_idx = ctx.block_statements[first_t + i];
                const auto& t_node = ctx.nodes[t_idx];

                if (t_node.type == NodeType::Identifier) {
                    std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
                    bool is_n = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
                    bool is_boxed = false;
                    bool is_loc = this->is_local(name, is_boxed);

                    if (is_local && !t_node.as.ident.is_global &&
                        CodeEmitter::g_reassigned_vars.count(std::string(name)) == 0 &&
                        i < v_count && !tmp_is_native[i]) {
                        uint32_t _alias_v_idx2 = ctx.block_statements[first_v + i];
                        if (_alias_v_idx2 < ctx.nodes.size() && ctx.nodes[_alias_v_idx2].type == NodeType::TableAccess) {
                            uint32_t _ht3 = ctx.nodes[_alias_v_idx2].as.table_access.table;
                            uint32_t _hk3 = ctx.nodes[_alias_v_idx2].as.table_access.key;
                            if (_ht3 < ctx.nodes.size() && _hk3 < ctx.nodes.size() &&
                                ctx.nodes[_ht3].type == NodeType::Identifier && ctx.nodes[_hk3].type == NodeType::String) {
                                std::string_view _hm3(ctx.nodes[_ht3].as.ident.name, ctx.nodes[_ht3].as.ident.length);
                                std::string_view _hf3(ctx.nodes[_hk3].as.string.text, ctx.nodes[_hk3].as.string.length);
                                            const char* _cf3 = lookup_builtin(_hm3, _hf3);
                                            if (_cf3)
                                                CodeEmitter::g_builtin_aliases[std::string(name)] = _cf3;
                            }
                        }
                    }

                    if (is_local || is_loc) {
                        if (is_local && is_boxed) out << "(*l_" << name << ") = " << val_str << ";\n";
                        else if (!is_local && is_boxed) out << "(*l_" << name << ") = " << val_str << ";\n";
                        else if (is_n) out << "l_" << name << " = " << num_str << ";\n";
                        else out << "l_" << name << " = " << val_str << ";\n";
                    } else if (is_global && v_count == 0) {
                    } else if (t_node.as.ident.is_global) {
                        out << "clx::set_env_var(L, _ENV, \"" << name << "\", " << val_str << ");\n";
                    } else {
                        size_t idx = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name));
                        out << "L->_G->settable(cstr_[" << idx << "], " << val_str << ");\n";
                    }
                } else if (t_node.type == NodeType::TableAccess) {
                    std::string_view t_name;
                    if (ctx.nodes[t_node.as.table_access.table].type == NodeType::Identifier) {
                        t_name = std::string_view(ctx.nodes[t_node.as.table_access.table].as.ident.name, ctx.nodes[t_node.as.table_access.table].as.ident.length);
                    }

                    if (!t_name.empty() && CodeEmitter::g_pure_numeric_arrays.count(t_name)) {
                        out << "l_" << t_name << "[static_cast<size_t>(";
                        emit_native(emit_native, t_node.as.table_access.key);
                        out << ") - 1] = " << num_str << ";\n";
                    } else if (CodeEmitter::g_bce_safe_nodes.count(t_idx)) {
                        out << "static_cast<clx::LTable*>(("; emit_node(t_node.as.table_access.table); out << ").as_pointer())->array[static_cast<size_t>(";
                        emit_native(emit_native, t_node.as.table_access.key);
                        out << ") - 1] = " << val_str << ";\n";
                    } else {
                        bool key_is_native = false;
                        uint32_t k_idx = t_node.as.table_access.key;
                        if (yields_number(ctx, k_idx, nullptr, CodeEmitter::g_current_fast_func)) key_is_native = true;

                        if (key_is_native) {
                            out << "{ clx::LTable* _t = static_cast<clx::LTable*>(("; emit_node(t_node.as.table_access.table); out << ").as_pointer()); size_t _k = static_cast<size_t>("; emit_native(emit_native, k_idx); out << "); if (_k - 1 < _t->array_size) _t->array[_k - 1] = " << val_str << "; else clx::table_set_int(L, clx::LValue(clx::LType::Table, _t), _k, " << val_str << "); }\n";
                        } else {
                            uint32_t kt2 = t_node.as.table_access.key;
                            if (kt2 < ctx.nodes.size() && ctx.nodes[kt2].type == NodeType::String) {
                                uint32_t tbl_idx = t_node.as.table_access.table;
                                bool tbl_is_stable = tbl_idx < ctx.nodes.size() &&
                                                     ctx.nodes[tbl_idx].type == NodeType::Identifier &&
                                                     ctx.nodes[tbl_idx].as.ident.is_global;
                                int _cs_i = (tbl_is_stable && CodeEmitter::g_cs_index < CodeEmitter::g_cs_max) ? CodeEmitter::g_cs_index++ : -1;
                                if (_cs_i >= 0) {
                                    out << "clx::table_set_cs(L, "; emit_node(t_node.as.table_access.table); out << ", cstr_[" << (std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(ctx.nodes[kt2].as.string.text, ctx.nodes[kt2].as.string.length)))) << "], " << val_str << ", &__cs[" << _cs_i << "]);\n";
                                } else {
                                    out << "clx::table_set(L, "; emit_node(t_node.as.table_access.table); out << ", cstr_[" << (std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(ctx.nodes[kt2].as.string.text, ctx.nodes[kt2].as.string.length)))) << "], " << val_str << ");\n";
                                }
                            } else {
                                out << "clx::table_set(L, "; emit_node(t_node.as.table_access.table); out << ", "; emit_node(t_node.as.table_access.key); out << ", " << val_str << ");\n";
                            }
                        }
                    }
                }
            }

            if (is_local) {
                for (size_t i = 0; i < t_count; ++i) {
                    uint32_t t_idx = ctx.block_statements[first_t + i];
                    const auto& t_node = ctx.nodes[t_idx];
                    if (t_node.type == NodeType::Identifier && t_node.as.ident.attr == clx::Attribute::Close) {
                        std::string_view name(t_node.as.ident.name, t_node.as.ident.length);
                        bool is_n = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
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
            break;
        }

        //------------------ NodeType::DoStatement: emits a do-end block
        case NodeType::DoStatement: {
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            size_t prev_locals = locals.size();
            if (node.as.do_stmt.body_block != 0xFFFFFFFF) emit_node(node.as.do_stmt.body_block);
            locals.resize(prev_locals);
            break;
        }
        //------------------ NodeType::UnaryOp: emits a unary operation
        case NodeType::UnaryOp:
            if (node.as.unary_op.op == static_cast<int>(UnaryOp::Len)) {

                std::string_view _len_tname;
                if (ctx.nodes[node.as.unary_op.expr].type == NodeType::Identifier) {
                    _len_tname = std::string_view(ctx.nodes[node.as.unary_op.expr].as.ident.name, ctx.nodes[node.as.unary_op.expr].as.ident.length);
                }
                if (!_len_tname.empty() && CodeEmitter::g_pure_numeric_arrays.count(_len_tname)) {
                    out << "clx::LValue(static_cast<int64_t>(l_" << _len_tname << ".size()))";
                } else {
                    out << "clx::len(L, "; emit_node(node.as.unary_op.expr); out << ")";
                }
            }
            if (node.as.unary_op.op == static_cast<int>(UnaryOp::Minus)) {
                if (yields_number(ctx, node.as.unary_op.expr, nullptr, CodeEmitter::g_current_fast_func)) {
                    if (ctx.nodes[node.as.unary_op.expr].type == NodeType::Integer) {
                        int64_t iv = ctx.nodes[node.as.unary_op.expr].as.integer.val;
                        out << "clx::integer(static_cast<int64_t>(-(" << iv << "ll)))";
                    } else {
                        out << "clx::LValue(static_cast<double>(-("; emit_native(emit_native, node.as.unary_op.expr); out << ")))";
                    }
                } else {
                    out << "clx::unm(L, "; emit_node(node.as.unary_op.expr); out << ")";
                }
            }
            if (node.as.unary_op.op == static_cast<int>(UnaryOp::BNot)) { out << "clx::bnot(L, "; emit_node(node.as.unary_op.expr); out << ")"; }
            if (node.as.unary_op.op == static_cast<int>(UnaryOp::Not)) { out << "clx::logical_not("; emit_node(node.as.unary_op.expr); out << ")";  }
            break;

        //------------------ NodeType::BinaryOp: emits a binary operation
        case NodeType::BinaryOp: {
            int op = node.as.bin_op.op;

            bool left_native = yields_number(ctx, node.as.bin_op.left, nullptr, CodeEmitter::g_current_fast_func);
            bool right_native = yields_number(ctx, node.as.bin_op.right, nullptr, CodeEmitter::g_current_fast_func);

            if (left_native && right_native) {
                 if (op == static_cast<int>(BinaryOp::Add)) { out << "clx::LValue(static_cast<double>("; emit_native(emit_native, node.as.bin_op.left); out << " + "; emit_native(emit_native, node.as.bin_op.right); out << "))"; break; }
                 if (op == static_cast<int>(BinaryOp::Sub)) { out << "clx::LValue(static_cast<double>("; emit_native(emit_native, node.as.bin_op.left); out << " - "; emit_native(emit_native, node.as.bin_op.right); out << "))"; break; }
                 if (op == static_cast<int>(BinaryOp::Mul)) { out << "clx::LValue(static_cast<double>("; emit_native(emit_native, node.as.bin_op.left); out << " * "; emit_native(emit_native, node.as.bin_op.right); out << "))"; break; }
                 if (op == static_cast<int>(BinaryOp::Div)) { out << "clx::LValue(static_cast<double>("; emit_native(emit_native, node.as.bin_op.left); out << " / "; emit_native(emit_native, node.as.bin_op.right); out << "))"; break; }
                 if (op == static_cast<int>(BinaryOp::Mod)) { out << "clx::LValue(static_cast<double>(std::fmod("; emit_native(emit_native, node.as.bin_op.left); out << ", "; emit_native(emit_native, node.as.bin_op.right); out << ")))"; break; }
                 if (op == static_cast<int>(BinaryOp::FloorDiv)) { out << "clx::LValue(static_cast<double>(std::floor(("; emit_native(emit_native, node.as.bin_op.left); out << ") / ("; emit_native(emit_native, node.as.bin_op.right); out << "))))"; break; }
                 if (op >= static_cast<int>(BinaryOp::Eq) && op <= static_cast<int>(BinaryOp::Ne)) {
                     static const char* ops[] = {"", "", "", "", "", " == ", " < ", " > ", " <= ", " >= ", " != "};
                     out << "clx::LValue("; emit_native(emit_native, node.as.bin_op.left); out << ops[op]; emit_native(emit_native, node.as.bin_op.right); out << ")";
                     break;
                 }
            }

            if (op == static_cast<int>(BinaryOp::Mod)) { out << "clx::mod(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::FloorDiv)) { out << "clx::idiv(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::Pow)) { out << "clx::pow(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::Concat)) {
                std::vector<uint32_t> operands;
                auto collect = [&](auto& self, uint32_t n_idx) -> void {
                    while (n_idx != 0xFFFFFFFF && ctx.nodes[n_idx].type == NodeType::ParenExpression) {
                        n_idx = ctx.nodes[n_idx].as.paren_expr.expr;
                    }
                    if (ctx.nodes[n_idx].type == NodeType::BinaryOp && ctx.nodes[n_idx].as.bin_op.op == static_cast<int>(BinaryOp::Concat)) {
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
                    } else if (yields_number(ctx, op, nullptr, CodeEmitter::g_current_fast_func)) {
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
                            out << "clx_memcpy(_p, \"" << cpp_escape(d) << "\", " << d.length() << "); _p += " << d.length() << "; ";
                        } else {
                            out << "_p += std::snprintf(_p, " << (total_str_len + 1) << " - (_p - _buf), \"%g\", static_cast<double>("; emit_native(emit_native, op); out << ")); ";
                        }
                    }
                    out << "*_p = '\\0'; return clx::LValue(L->intern_string(_buf, static_cast<size_t>(_p - _buf))); }())";
                } else {
                    out << "([&](){\n";
                    out << "    clx::LValue args[] = {";
                    for (size_t i = 0; i < operands.size(); ++i) {
                        emit_node(operands[i]);
                        if (i < operands.size() - 1) out << ", ";
                    }
                    out << "};\n";
                    out << "    return clx::concat_multi(L, args, " << operands.size() << ");\n";
                    out << "}())";
                }
                break;
            }
            if (op == static_cast<int>(BinaryOp::And)) {
                out << "([&](){ clx::LValue _a = "; emit_node(node.as.bin_op.left);
                out << "; return _a.as_bool() ? ("; emit_node(node.as.bin_op.right);
                out << ") : _a; }())";
                break;
            }
            if (op == static_cast<int>(BinaryOp::Or)) {
                out << "([&](){ clx::LValue _a = "; emit_node(node.as.bin_op.left);
                out << "; return _a.as_bool() ? _a : ("; emit_node(node.as.bin_op.right);
                out << "); }())";
                break;
            }

            if (op == static_cast<int>(BinaryOp::BitAnd)) { out << "clx::band(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::BitOr)) { out << "clx::bor(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::BitXor)) { out << "clx::bxor(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::Shl)) { out << "clx::shl(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }
            if (op == static_cast<int>(BinaryOp::Shr)) { out << "clx::shr(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")"; break; }

            if (op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div)) {
                static const char* fn[] = {"", "add", "sub", "mul", "div"};
                out << "clx::" << fn[op] << "(L, "; emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); out << ")";
                break;
            }

            if (op >= static_cast<int>(BinaryOp::Eq) && op <= static_cast<int>(BinaryOp::Ne)) {
                static const char* fn[] = {"", "", "", "", "", "eq", "lt", "lt", "le", "le", "eq"};
                if (op == static_cast<int>(BinaryOp::Ne)) out << "clx::LValue(!(";
                out << "clx::" << fn[op] << "(L, ";
                if (op == static_cast<int>(BinaryOp::Gt) || op == static_cast<int>(BinaryOp::Ge)) { emit_node(node.as.bin_op.right); out << ", "; emit_node(node.as.bin_op.left); }
                else { emit_node(node.as.bin_op.left); out << ", "; emit_node(node.as.bin_op.right); }
                out << ")";
                if (op == static_cast<int>(BinaryOp::Ne)) out << ").as_bool())";
            } else {
                static const char* op_strings[] = { "", " + ", " - ", " * ", " / ", " == ", " < ", " > ", " <= ", " >= ", " != " };
                out << "(";
                emit_node(node.as.bin_op.left);
                out << op_strings[op];
                emit_node(node.as.bin_op.right);
                out << ")";
            }
            break;
        }
        //------------------ NodeType::TrueLiteral: emits a true literal
        case NodeType::TrueLiteral:  out << "clx::LValue(true)"; break;
        //------------------ NodeType::FalseLiteral: emits a false literal
        case NodeType::FalseLiteral: out << "clx::LValue(false)"; break;
        //------------------ NodeType::NilLiteral: emits a nil literal
        case NodeType::NilLiteral:   out << "clx::LValue()"; break;

        //------------------ NodeType::Number: emits a numeric literal
        case NodeType::Number:
            out << "clx::LValue(static_cast<double>(" << node.as.number.val << "))";
            break;

        //------------------ NodeType::Integer: emits an integer literal
        case NodeType::Integer:
            out << "clx::integer(static_cast<int64_t>(" << node.as.integer.val << "))";
            break;

        //------------------ NodeType::Identifier: emits a variable reference
        case NodeType::Identifier: {
            std::string_view name(node.as.ident.name, node.as.ident.length);
            bool is_native = std::find(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), name) != CodeEmitter::g_native_numbers.end();
            bool is_boxed = false;
            bool is_loc = this->is_local(name, is_boxed);

            if (node.as.ident.is_global) {
                if (CodeEmitter::g_global_constants.count(name)) {
                    out << "clx::LValue(static_cast<double>(" << CodeEmitter::g_global_constants[name] << "))";
                } else if (CodeEmitter::g_string_builders.count(name)) {
                    out << "(sb_" << name << ".empty() ? clx::get_env_var(L, _ENV, \"" << name << "\") : clx::LValue(sb_" << name << ".to_string(L)))";
                } else {
                    out << "clx::get_env_var(L, _ENV, \"" << name << "\")";
                }
            } else if (is_native && !is_boxed && !node.as.ident.is_captured) {
                out << "clx::LValue(l_" << name << ")";
            } else if (is_loc) {
                if (CodeEmitter::g_string_builders.count(name)) {
                    if (is_boxed) {
                        out << "(sb_" << name << ".empty() ? (*l_" << name << ") : clx::LValue(sb_" << name << ".to_string(L)))";
                    } else {
                        out << "(sb_" << name << ".empty() ? l_" << name << " : clx::LValue(sb_" << name << ".to_string(L)))";
                    }
                } else if (is_boxed) out << "(*l_" << name << ")";
                else if (is_native) out << "clx::LValue(l_" << name << ")";
                else out << "l_" << name;
            } else {
                size_t idx = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), name));
                out << "clx_gettable_safe(L->_G->gettable(cstr_[" << idx << "]))";
            }
            break;
        }

        //------------------ NodeType::String: emits a string literal
        case NodeType::String: {
            std::string_view s(node.as.string.text, node.as.string.length);
            size_t idx = std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), s));
            out << "cstr_[" << idx << "]";
            break;
        }

        //------------------ NodeType::IfStatement: emits an if-then-else statement
        case NodeType::IfStatement:
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            out << "if (";
            emit_condition(emit_condition, node.as.if_stmt.condition);
            out << ")\n";
            if (node.as.if_stmt.then_block != 0xFFFFFFFF) emit_node(node.as.if_stmt.then_block);
            if (node.as.if_stmt.else_block != 0xFFFFFFFF) {
                out << "else\n";
                emit_node(node.as.if_stmt.else_block);
            }
            break;

        //------------------ NodeType::WhileStatement: emits a while loop
        case NodeType::WhileStatement:
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            out << "while (";
            emit_condition(emit_condition, node.as.while_stmt.condition);
            out << ")\n";
            if (node.as.while_stmt.body_block != 0xFFFFFFFF) emit_node(node.as.while_stmt.body_block);
            break;

        //------------------ NodeType::RepeatStatement: emits a repeat-until loop
        case NodeType::RepeatStatement:
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            out << "do\n";
            if (node.as.repeat_stmt.body_block != 0xFFFFFFFF) emit_node(node.as.repeat_stmt.body_block);
            out << "while (!(";
            emit_condition(emit_condition, node.as.repeat_stmt.condition);
            out << "));\n";
            break;

        //------------------ NodeType::ForStatement: emits a numeric for loop
        case NodeType::ForStatement: {
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";

            bool native_for = yields_number(ctx, node.as.for_stmt.start_expr, nullptr, CodeEmitter::g_current_fast_func) &&
                              yields_number(ctx, node.as.for_stmt.limit_expr, nullptr, CodeEmitter::g_current_fast_func) &&
                              (node.as.for_stmt.step_expr == 0xFFFFFFFF || yields_number(ctx, node.as.for_stmt.step_expr, nullptr, CodeEmitter::g_current_fast_func));

            out << "{\n";
            if (!native_for) out << "clx::ScopeGuard _sg_for_" << node_idx << "(L);\n";

            if (native_for) {
                out << "double s_" << node_idx << " = "; emit_native(emit_native, node.as.for_stmt.start_expr); out << ";\n";
                out << "double l_" << node_idx << " = "; emit_native(emit_native, node.as.for_stmt.limit_expr); out << ";\n";
                if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
                    out << "double st_" << node_idx << " = "; emit_native(emit_native, node.as.for_stmt.step_expr); out << ";\n";
                } else {
                    out << "double st_" << node_idx << " = 1.0;\n";
                }
            } else {
                out << "clx::LValue start_" << node_idx << " = "; emit_node(node.as.for_stmt.start_expr); out << ";\n";
                out << "L->shadow_stack[L->shadow_top++] = &start_" << node_idx << ";\n";
                out << "clx::LValue limit_" << node_idx << " = "; emit_node(node.as.for_stmt.limit_expr); out << ";\n";
                out << "L->shadow_stack[L->shadow_top++] = &limit_" << node_idx << ";\n";

                if (node.as.for_stmt.step_expr != 0xFFFFFFFF) {
                    out << "clx::LValue step_" << node_idx << " = "; emit_node(node.as.for_stmt.step_expr); out << ";\n";
                } else {
                    out << "clx::LValue step_" << node_idx << " = clx::LValue(static_cast<double>(1.0));\n";
                }
                out << "L->shadow_stack[L->shadow_top++] = &step_" << node_idx << ";\n";

                out << "double s_" << node_idx << ", l_" << node_idx << ", st_" << node_idx << ";\n";
                out << "if (!start_" << node_idx << ".to_number(s_" << node_idx << ") || !limit_" << node_idx << ".to_number(l_" << node_idx << ") || !step_" << node_idx << ".to_number(st_" << node_idx << ")) {\n";
                out << "std::cerr << \"Error: " << ctx.filename << ":" << node.line << ": 'for' initial values must be numeric\\n\"; std::exit(1);\n}\n";
            }

            std::string_view var_name(ctx.nodes[node.as.for_stmt.var_ident].as.ident.name, ctx.nodes[node.as.for_stmt.var_ident].as.ident.length);
            bool is_cap = ctx.nodes[node.as.for_stmt.var_ident].as.ident.is_captured;
            bool is_n = !is_cap;


            bool step_is_default = (node.as.for_stmt.step_expr == 0xFFFFFFFF);
            bool step_known_positive = step_is_default;
            bool step_known_negative = false;
            int64_t step_int_val = 1;
            if (!step_is_default) {
                auto& step_node = ctx.nodes[node.as.for_stmt.step_expr];
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

            bool counter_is_int = native_for && !is_cap &&
                ctx.nodes[node.as.for_stmt.start_expr].type == NodeType::Integer &&
                (step_is_default || (ctx.nodes[node.as.for_stmt.step_expr].type == NodeType::Integer && step_int_val == 1));


            auto _saved_hoisted = std::move(CodeEmitter::g_hoisted_lookups);
            auto _saved_hoisted_cf = std::move(CodeEmitter::g_hoisted_cfuncs);
            CodeEmitter::g_hoisted_lookups.clear();
            CodeEmitter::g_hoisted_cfuncs.clear();
            std::vector<uint32_t> _invariant_lookups;
            auto _find_invariant = [&](auto& self, uint32_t bn_idx) -> void {
                if (bn_idx == 0xFFFFFFFF || bn_idx >= ctx.nodes.size()) return;
                auto& bn = ctx.nodes[bn_idx];
                if (bn.type == NodeType::TableAccess) {
                    uint32_t tbl = bn.as.table_access.table;
                    uint32_t key = bn.as.table_access.key;
                    if (tbl < ctx.nodes.size() && key < ctx.nodes.size() &&
                        ctx.nodes[tbl].type == NodeType::Identifier &&
                        ctx.nodes[tbl].as.ident.is_global &&
                        ctx.nodes[key].type == NodeType::String) {
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
            _invariant_lookups.erase(std::unique(_invariant_lookups.begin(), _invariant_lookups.end()), _invariant_lookups.end());
            for (uint32_t _h_node : _invariant_lookups) {
                std::string _h_name = "_hoist_" + std::to_string(_h_node);
                int _cs_i = (CodeEmitter::g_cs_index < CodeEmitter::g_cs_max) ? CodeEmitter::g_cs_index++ : -1;
                if (_cs_i >= 0) {
                    out << "clx::LValue " << _h_name << " = clx::table_get_cs(L, ";
                    emit_node(ctx.nodes[_h_node].as.table_access.table);
                    out << ", "; emit_node(ctx.nodes[_h_node].as.table_access.key);
                    out << ", &__cs[" << _cs_i << "]);\n";
                } else {
                    out << "clx::LValue " << _h_name << " = clx::table_get(L, ";
                    emit_node(ctx.nodes[_h_node].as.table_access.table);
                    out << ", "; emit_node(ctx.nodes[_h_node].as.table_access.key);
                    out << ");\n";
                }
                out << "L->shadow_stack[L->shadow_top++] = &" << _h_name << ";\n";
                CodeEmitter::g_hoisted_lookups[_h_node] = _h_name;
                uint32_t _ht = ctx.nodes[_h_node].as.table_access.table;
                uint32_t _hk = ctx.nodes[_h_node].as.table_access.key;
                if (_ht < ctx.nodes.size() && _hk < ctx.nodes.size() &&
                    ctx.nodes[_ht].type == NodeType::Identifier && ctx.nodes[_hk].type == NodeType::String) {
                    std::string_view _hm(ctx.nodes[_ht].as.ident.name, ctx.nodes[_ht].as.ident.length);
                    std::string_view _hf(ctx.nodes[_hk].as.string.text, ctx.nodes[_hk].as.string.length);
                    const char* _cf = lookup_builtin(_hm, _hf);
                    if (_cf)
                        CodeEmitter::g_hoisted_cfuncs[_h_name] = _cf;
                }
            }

            auto emit_for_body = [&]() {
                if (is_cap || !is_n) {
                    out << "clx::ScopeGuard _sg_iter(L);\n";
                }

                if (is_cap) {
                    out << "clx::LUpValue l_" << var_name << ";\n";
                    out << "l_" << var_name << " = clx::make_upvalue(clx::LValue(i_val));\n";
                    out << "L->shadow_stack[L->shadow_top++] = l_" << var_name << ".get();\n";
                } else if (is_n) {
                    if (counter_is_int) {
                        out << "const int64_t l_" << var_name << " = i_val;\n";
                        CodeEmitter::g_native_integers.insert(var_name);
                        CodeEmitter::g_native_numbers.push_back(var_name);
                    } else {
                        out << "const double l_" << var_name << " = i_val;\n";
                        CodeEmitter::g_native_numbers.push_back(var_name);
                    }
                } else {
                    out << "const clx::LValue l_" << var_name << "( (i_val == static_cast<int64_t>(i_val)) ? clx::LValue(static_cast<int64_t>(i_val)) : clx::LValue(i_val) );\n";
                }

                size_t prev_locals = locals.size();
                locals.push_back({var_name, is_cap});
                if (node.as.for_stmt.body_block != 0xFFFFFFFF) {
                    bool prev_skip = CodeEmitter::g_skip_block_braces;
                    CodeEmitter::g_skip_block_braces = true;
                    if (CodeEmitter::g_in_fast_function && node.as.for_stmt.body_block != 0xFFFFFFFF) {
                        bool _body_has_goto = false;
                        auto _check_goto = [&](auto& self, uint32_t n_idx) -> void {
                            if (n_idx == 0xFFFFFFFF || n_idx >= ctx.nodes.size() || _body_has_goto) return;
                            auto& _n = ctx.nodes[n_idx];
                            if (_n.type == NodeType::GotoStatement) { _body_has_goto = true; return; }
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
                    CodeEmitter::g_skip_block_braces = prev_skip;
                }
                locals.resize(prev_locals);
            };

            if (counter_is_int) {
                int64_t start_int = ctx.nodes[node.as.for_stmt.start_expr].as.integer.val;
                if (step_known_positive) {
                    out << "for (int64_t i_val = " << start_int << "; i_val <= static_cast<int64_t>(l_" << node_idx << "); i_val++) {\n";
                    emit_for_body();
                    out << "}\n";
                } else {
                    out << "for (int64_t i_val = " << start_int << "; i_val >= static_cast<int64_t>(l_" << node_idx << "); i_val--) {\n";
                    emit_for_body();
                    out << "}\n";
                }
            } else if (step_known_positive) {
                out << "for (double i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_" << node_idx << ") {\n";
                emit_for_body();
                out << "}\n";
            } else if (step_known_negative) {
                out << "for (double i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_" << node_idx << ") {\n";
                emit_for_body();
                out << "}\n";
            } else {
                out << "if (st_" << node_idx << " > 0) {\n";
                out << "    for (double i_val = s_" << node_idx << "; i_val <= l_" << node_idx << "; i_val += st_" << node_idx << ") {\n";
                emit_for_body();
                out << "    }\n";
                out << "} else {\n";
                out << "    for (double i_val = s_" << node_idx << "; i_val >= l_" << node_idx << "; i_val += st_" << node_idx << ") {\n";
                emit_for_body();
                out << "    }\n";
                out << "}\n";
            }

            for (size_t _hi = 0; _hi < CodeEmitter::g_hoisted_lookups.size(); ++_hi)
                out << "L->shadow_top--;\n";
            CodeEmitter::g_hoisted_lookups = std::move(_saved_hoisted);
            CodeEmitter::g_hoisted_cfuncs = std::move(_saved_hoisted_cf);
            out << "}\n";
            break;
        }

        //------------------ NodeType::GenericForStatement: emits a generic for loop
        case NodeType::GenericForStatement: {
            out << "#line " << node.line << " \"" << ctx.filename << "\"\n";
            const auto& loop = node.as.generic_for;

            out << "{\n    clx::ScopeGuard _sg_gen_for_" << node_idx << "(L);\n";

            out << "    clx::MultiValue _triplet_" << node_idx << ";\n";
            if (loop.iter_count > 0 && ctx.nodes[ctx.block_statements[loop.first_iter]].type == NodeType::CallExpression) {
                uint32_t iter_node = ctx.block_statements[loop.first_iter];
                const auto& call_node = ctx.nodes[iter_node];
                bool is_direct = false;
                std::string_view fname;
                uint32_t tgt = call_node.as.call_expr.target;
                if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
                    fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                    if (CodeEmitter::g_direct_callables.count(fname)) is_direct = true;
                }

                out << "    {\n";
                if (call_node.as.call_expr.arg_count > 0) {
                    out << "        clx::LValue args_" << iter_node << "[] = {";
                    for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                        emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                        if (a < call_node.as.call_expr.arg_count - 1) out << ", ";
                    }
                    out << "};\n";
                }

                if (is_direct) {
                    out << "        _triplet_" << node_idx << " = _impl_" << fname << "(L, " << (call_node.as.call_expr.arg_count > 0 ? "args_" + std::to_string(iter_node) : "nullptr") << ", " << call_node.as.call_expr.arg_count << ");\n";
                } else {
                    if (call_node.as.call_expr.arg_count > 0) {
                        out << "        for (size_t i = 0; i < " << call_node.as.call_expr.arg_count << "; ++i) L->shadow_stack[L->shadow_top++] = &args_" << iter_node << "[i];\n";
                    }
                    out << "        _triplet_" << node_idx << " = clx::call_function(L, ";
                    emit_node(tgt);
                    out << ", " << (call_node.as.call_expr.arg_count > 0 ? "args_" + std::to_string(iter_node) : "nullptr") << ", " << call_node.as.call_expr.arg_count << ", \"" << ctx.filename << "\", " << call_node.line << ");\n";
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

            out << "    clx::LValue _f_" << node_idx << " = (_triplet_" << node_idx << ".count > 0) ? _triplet_" << node_idx << "[0] : clx::LValue();\n";
            out << "    clx::LValue _s_" << node_idx << " = (_triplet_" << node_idx << ".count > 1) ? _triplet_" << node_idx << "[1] : clx::LValue();\n";
            out << "    clx::LValue _var_" << node_idx << " = (_triplet_" << node_idx << ".count > 2) ? _triplet_" << node_idx << "[2] : clx::LValue();\n";

            out << "    while (true) {\n";
            out << "        clx::LValue _args_" << node_idx << "[] = { _s_" << node_idx << ", _var_" << node_idx << " };\n";
            out << "        clx::LCFunction* _lcf_" << node_idx << " = static_cast<clx::LCFunction*>(_f_" << node_idx << ".as_pointer());\n";
out << "        clx::MultiValue _res_" << node_idx << " = _lcf_" << node_idx << "->direct ? _lcf_" << node_idx << "->direct(L, _args_" << node_idx << ", 2) : _lcf_" << node_idx << "->func(L, _args_" << node_idx << ", 2);\n";

            out << "        _var_" << node_idx << " = (_res_" << node_idx << ".count > 0) ? _res_" << node_idx << "[0] : clx::LValue();\n";
            out << "        if (_var_" << node_idx << ".type() == clx::LType::Nil) break;\n";

            size_t prev_locals = locals.size();
            for (uint32_t i = 0; i < loop.var_count; ++i) {
                uint32_t v_idx = ctx.block_statements[loop.first_var + i];
                std::string_view v_name(ctx.nodes[v_idx].as.ident.name, ctx.nodes[v_idx].as.ident.length);
                bool is_cap = ctx.nodes[v_idx].as.ident.is_captured;

                CodeEmitter::g_native_numbers.erase(
                    std::remove(CodeEmitter::g_native_numbers.begin(), CodeEmitter::g_native_numbers.end(), v_name),
                    CodeEmitter::g_native_numbers.end()
                );
                CodeEmitter::g_native_integers.erase(v_name);

                if (is_cap) {
                    out << "        clx::LUpValue l_" << v_name << ";\n";
                    out << "        l_" << v_name << " = clx::make_upvalue((_res_" << node_idx << ".count > " << i << ") ? _res_" << node_idx << "[" << i << "] : clx::LValue());\n";
                    out << "        L->shadow_stack[L->shadow_top++] = l_" << v_name << ".get();\n";
                } else {
                    out << "        clx::LValue l_" << v_name << " = (_res_" << node_idx << ".count > " << i << ") ? _res_" << node_idx << "[" << i << "] : clx::LValue();\n";
                    out << "        L->shadow_stack[L->shadow_top++] = &l_" << v_name << ";\n";
                }
                locals.push_back({v_name, is_cap});
            }

            if (loop.body_block != 0xFFFFFFFF) emit_node(loop.body_block);

            locals.resize(prev_locals);
            out << "    }\n}\n";
            break;
        }

        //------------------ NodeType::TableConstructor: emits a table constructor
        case NodeType::TableConstructor: {
            bool _has_va = false;
            for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
                uint32_t v = ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1];
                if (ctx.nodes[v].type == NodeType::Vararg || ctx.nodes[v].type == NodeType::CallExpression) { _has_va = true; break; }
            }
            out << "([&]() {\nclx::LValue _t = L->create_table(";
            if (CodeEmitter::g_table_presize.count(node_idx)) {
                uint32_t nidx = CodeEmitter::g_table_presize[node_idx];
                auto check_declared = [&](auto& self, uint32_t ni) -> bool {
                    if (ni >= ctx.nodes.size()) return true;
                    const auto& n = ctx.nodes[ni];
                    if (n.type == NodeType::Identifier) {
                        std::string_view nm(n.as.ident.name, n.as.ident.length);
                        bool dummy = false;
                        return this->is_local(nm, dummy);
                    }
                    if (n.type == NodeType::BinaryOp) return self(self, n.as.bin_op.left) && self(self, n.as.bin_op.right);
                    if (n.type == NodeType::UnaryOp) return self(self, n.as.unary_op.expr);
                    if (n.type == NodeType::Number || n.type == NodeType::Integer) return true;
                    return false;
                };
                if (check_declared(check_declared, nidx)) {
                    out << "static_cast<size_t>("; emit_native(emit_native, nidx); out << ")";
                } else {
                    out << node.as.table_cons.count;
                }
            } else {
                out << node.as.table_cons.count;
            }
            out << ");\nL->shadow_stack[L->shadow_top++] = &_t;\n";
            if (_has_va) out << "size_t _ai = 1;\n";
            uint32_t array_index = 1;

            for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
                uint32_t k = ctx.block_statements[node.as.table_cons.first_item + i * 2];
                uint32_t v = ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1];
                if (i == node.as.table_cons.count - 1 && (ctx.nodes[v].type == NodeType::Vararg || ctx.nodes[v].type == NodeType::CallExpression)) {
                    CodeEmitter::g_expect_multivalue = true;
                    out << "clx::MultiValue _mret_" << node_idx << " = ";
                    emit_node(v);
                    out << ";\n";
                    CodeEmitter::g_expect_multivalue = false;
                    out << "for (size_t _vi = 0; _vi < _mret_" << node_idx << ".count; ++_vi) {\n";
                    out << "  static_cast<clx::LTable*>(_t.as_pointer())->settable(";
                    out << "clx::LValue(static_cast<double>(_ai++)), _mret_" << node_idx << "[_vi]);\n";
                    out << "}\n";
                } else {
                    out << "static_cast<clx::LTable*>(_t.as_pointer())->settable(";
                    if (k == 0xFFFFFFFF) {
                        out << "clx::LValue(static_cast<double>(" << (_has_va ? "_ai++" : std::to_string(array_index++)) << "))";
                    } else {
                        emit_node(k);
                    }
                    out << ", ";
                    emit_node(v);
                    out << ");\n";
                }
            }
            out << "L->shadow_top--;\nreturn _t;\n}())";
            break;
        }

        //------------------ NodeType::TableAccess: emits a table indexing expression
        case NodeType::TableAccess: {
            {
                auto hit = CodeEmitter::g_hoisted_lookups.find(node_idx);
                if (hit != CodeEmitter::g_hoisted_lookups.end()) {
                    out << hit->second;
                    break;
                }
            }
            std::string_view t_name;
            if (ctx.nodes[node.as.table_access.table].type == NodeType::Identifier) {
                t_name = std::string_view(ctx.nodes[node.as.table_access.table].as.ident.name, ctx.nodes[node.as.table_access.table].as.ident.length);
            }
            if (!t_name.empty() && CodeEmitter::g_pure_numeric_arrays.count(t_name)) {
                out << "clx::LValue(static_cast<double>(l_" << t_name << "[static_cast<size_t>(";
                emit_native(emit_native, node.as.table_access.key);
                out << ") - 1]))";
                break;
            }
            if (CodeEmitter::g_bce_safe_nodes.count(node_idx)) {
                out << "(static_cast<clx::LTable*>(("; emit_node(node.as.table_access.table); out << ").as_pointer())->array[static_cast<size_t>(";
                emit_native(emit_native, node.as.table_access.key);
                out << ") - 1])";
            } else {
                bool key_is_native = false;
                uint32_t k_idx = node.as.table_access.key;
                if (yields_number(ctx, k_idx, nullptr, CodeEmitter::g_current_fast_func)) key_is_native = true;

                if (key_is_native) {
                    uint32_t _cb = node.as.table_access.table;
                    std::vector<uint32_t> _cks;
                    _cks.push_back(k_idx);
                    while (ctx.nodes[_cb].type == NodeType::TableAccess &&
                           yields_number(ctx, ctx.nodes[_cb].as.table_access.key, nullptr, CodeEmitter::g_current_fast_func)) {
                        _cks.push_back(ctx.nodes[_cb].as.table_access.key);
                        _cb = ctx.nodes[_cb].as.table_access.table;
                    }
                    if (_cks.size() > 1 && ctx.nodes[_cb].type == NodeType::Identifier) {
                        std::reverse(_cks.begin(), _cks.end());
                        out << "([&](){ clx::LTable* _tc = static_cast<clx::LTable*>((";
                        emit_node(_cb); out << ").as_pointer());";
                        for (size_t _i = 0; _i < _cks.size() - 1; ++_i) {
                            out << " size_t _k" << _i << " = static_cast<size_t>(";
                            emit_native(emit_native, _cks[_i]); out << ");";
                            out << " clx::LValue _v" << _i << " = (_k" << _i << " - 1 < _tc->array_size) ? _tc->array[_k" << _i << " - 1] : clx::table_get_int(L, clx::LValue(clx::LType::Table, _tc), _k" << _i << ");";
                            out << " _tc = static_cast<clx::LTable*>(_v" << _i << ".as_pointer());";
                        }
                        size_t _last = _cks.size() - 1;
                        out << " size_t _k" << _last << " = static_cast<size_t>(";
                        emit_native(emit_native, _cks[_last]); out << ");";
                        out << " return (_k" << _last << " - 1 < _tc->array_size) ? _tc->array[_k" << _last << " - 1] : clx::table_get_int(L, clx::LValue(clx::LType::Table, _tc), _k" << _last << "); }())";
                    } else {
                        out << "([&](){ clx::LTable* _t = static_cast<clx::LTable*>(("; emit_node(node.as.table_access.table); out << ").as_pointer()); size_t _k = static_cast<size_t>("; emit_native(emit_native, k_idx); out << "); return (_k - 1 < _t->array_size) ? _t->array[_k - 1] : clx::table_get_int(L, clx::LValue(clx::LType::Table, _t), _k); }())";
                    }
                } else {
                    uint32_t kt = node.as.table_access.key;
                    if (kt < ctx.nodes.size() && ctx.nodes[kt].type == NodeType::String) {
                        uint32_t tbl_idx = node.as.table_access.table;
                        bool tbl_is_stable = tbl_idx < ctx.nodes.size() &&
                                             ctx.nodes[tbl_idx].type == NodeType::Identifier &&
                                             ctx.nodes[tbl_idx].as.ident.is_global;
                        int _cs_i = (tbl_is_stable && CodeEmitter::g_cs_index < CodeEmitter::g_cs_max) ? CodeEmitter::g_cs_index++ : -1;
                        if (_cs_i >= 0) {
                            out << "clx::table_get_cs(L, "; emit_node(node.as.table_access.table); out << ", cstr_[" << (std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length)))) << "], &__cs[" << _cs_i << "])";
                        } else {
                            out << "clx::table_get(L, "; emit_node(node.as.table_access.table); out << ", cstr_[" << (std::distance(CodeEmitter::g_string_pool.begin(), std::find(CodeEmitter::g_string_pool.begin(), CodeEmitter::g_string_pool.end(), std::string_view(ctx.nodes[kt].as.string.text, ctx.nodes[kt].as.string.length)))) << "])";
                        }
                    } else {
                        out << "clx::table_get(L, "; emit_node(node.as.table_access.table); out << ", "; emit_node(node.as.table_access.key); out << ")";
                    }
                }
            }
            break;
        }

        //------------------ NodeType::Vararg: emits a vararg expression
        case NodeType::Vararg: {
            if (CodeEmitter::g_expect_multivalue) out << "clx::MultiValue(_va_args, _va_count)";
            else out << "(_va_count > 0 ? _va_args[0] : clx::LValue())";
            break;
        }

        //------------------ NodeType::BreakStatement: emits a break statement
        case NodeType::BreakStatement: {
            out << "break;";
            break;
        }
    }
}

}