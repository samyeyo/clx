// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  structs.cpp · Struct Local Promotion       │
// └─────────────────────────────────────────────┘

#include "passes.h"
#include "analysis_state.h"
#include "optimizer.h"
#include "../syntax/nodes.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace clx {

//------------------ kStructCppKeywords: field names that cannot be used as C++ struct members
static const std::set<std::string_view> kStructCppKeywords = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
    "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
    "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
};

//------------------ valid_member_name: Lua field name usable directly as a C++ struct member
static bool valid_member_name(std::string_view nm)
{
    if (nm.empty())
        return false;
    char c0 = nm[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
        return false;
    for (char c : nm) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return kStructCppKeywords.count(nm) == 0;
}

//------------------ StructCandidate: a local that passed qualification A-D, awaiting the kind fixpoint
struct StructCandidate {
    uint32_t owner = 0xFFFFFFFF;
    std::string name;
    uint32_t decl_stmt = 0xFFFFFFFF;
    uint32_t decl_ident = 0xFFFFFFFF;
    std::vector<std::string> fields;
    std::vector<bool> late;
    std::vector<uint32_t> ctor_values;
    std::vector<uint32_t> read_nodes;
    std::vector<std::string> read_fields;
    struct Write {
        uint32_t access;
        uint32_t value;
        std::string field;
    };
    std::vector<Write> writes;
};

//------------------ pass_struct_locals: promote record locals to native C++ struct types
void pass_struct_locals(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node)
{
    state.struct_locals.clear();
    state.struct_proven_reads.clear();
    state.struct_shapes.clear();
    if (ctx.nodes.empty() || root_node >= ctx.nodes.size())
        return;

    //------------------ parent map: mirrors traverse_node's edge list, recording each node's parent
    std::vector<uint32_t> parent(ctx.nodes.size(), 0xFFFFFFFF);
    std::function<void(uint32_t, uint32_t)> link = [&](uint32_t ni, uint32_t par) {
        if (ni == 0xFFFFFFFF || ni >= ctx.nodes.size())
            return;
        if (par != 0xFFFFFFFF && parent[ni] == 0xFFFFFFFF)
            parent[ni] = par;
        const auto &n = ctx.nodes[ni];
        auto ch = [&](uint32_t c) { link(c, ni); };
        if (n.type == NodeType::Block) {
            for (uint32_t i = 0; i < n.as.block.count; ++i)
                ch(ctx.block_statements[n.as.block.first_statement + i]);
        } else if (n.type == NodeType::LocalDecl || n.type == NodeType::GlobalDeclStatement
            || n.type == NodeType::Assignment) {
            uint32_t v_count = (n.type == NodeType::LocalDecl)
                ? n.as.local_decl.value_count
                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.value_count : n.as.assign.value_count);
            uint32_t f_value = (n.type == NodeType::LocalDecl)
                ? n.as.local_decl.first_value
                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_value : n.as.assign.first_value);
            uint32_t id_count = (n.type == NodeType::LocalDecl)
                ? n.as.local_decl.ident_count
                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.ident_count : n.as.assign.target_count);
            uint32_t f_ident = (n.type == NodeType::LocalDecl)
                ? n.as.local_decl.first_ident
                : ((n.type == NodeType::GlobalDeclStatement) ? n.as.global_decl.first_ident : n.as.assign.first_target);
            for (uint32_t i = 0; i < id_count; ++i)
                ch(ctx.block_statements[f_ident + i]);
            for (uint32_t i = 0; i < v_count; ++i)
                ch(ctx.block_statements[f_value + i]);
        } else if (n.type == NodeType::BinaryOp) {
            ch(n.as.bin_op.left);
            ch(n.as.bin_op.right);
        } else if (n.type == NodeType::UnaryOp) {
            ch(n.as.unary_op.expr);
        } else if (n.type == NodeType::CallExpression) {
            ch(n.as.call_expr.target);
            for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i)
                ch(ctx.block_statements[n.as.call_expr.first_arg + i]);
        } else if (n.type == NodeType::IntrinsicCall) {
            for (uint32_t i = 0; i < n.as.intrinsic_call.arg_count; ++i)
                ch(ctx.block_statements[n.as.intrinsic_call.first_arg + i]);
        } else if (n.type == NodeType::IfStatement) {
            ch(n.as.if_stmt.condition);
            ch(n.as.if_stmt.then_block);
            if (n.as.if_stmt.else_block != 0xFFFFFFFF)
                ch(n.as.if_stmt.else_block);
        } else if (n.type == NodeType::WhileStatement) {
            ch(n.as.while_stmt.condition);
            ch(n.as.while_stmt.body_block);
        } else if (n.type == NodeType::RepeatStatement) {
            ch(n.as.repeat_stmt.body_block);
            ch(n.as.repeat_stmt.condition);
        } else if (n.type == NodeType::ForStatement) {
            ch(n.as.for_stmt.start_expr);
            ch(n.as.for_stmt.limit_expr);
            ch(n.as.for_stmt.step_expr);
            ch(n.as.for_stmt.body_block);
        } else if (n.type == NodeType::GenericForStatement) {
            for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                ch(ctx.block_statements[n.as.generic_for.first_iter + i]);
            ch(n.as.generic_for.body_block);
        } else if (n.type == NodeType::DoStatement) {
            ch(n.as.do_stmt.body_block);
        } else if (n.type == NodeType::TableConstructor) {
            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                ch(ctx.block_statements[n.as.table_cons.first_item + i * 2]);
                ch(ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1]);
            }
        } else if (n.type == NodeType::TableAccess) {
            ch(n.as.table_access.table);
            ch(n.as.table_access.key);
        } else if (n.type == NodeType::ReturnStatement) {
            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
                ch(ctx.block_statements[n.as.return_stmt.first_value + i]);
        } else if (n.type == NodeType::FunctionDef) {
            ch(n.as.func_def.body_block);
        } else if (n.type == NodeType::ParenExpression) {
            ch(n.as.paren_expr.expr);
        }
    };
    link(root_node, 0xFFFFFFFF);

    //------------------ assignment-target set, per-owner decl counts, per-owner reassigned names
    std::vector<uint8_t> is_assign_target(ctx.nodes.size(), 0);
    std::vector<uint8_t> is_decl_ident(ctx.nodes.size(), 0);
    std::map<std::pair<uint32_t, std::string_view>, uint32_t> decl_counts;
    std::map<uint32_t, std::set<std::string_view>> reassigned;
    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto &n = ctx.nodes[i];
        if (n.type == NodeType::Assignment) {
            for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti) {
                uint32_t tgt = ctx.block_statements[n.as.assign.first_target + ti];
                if (tgt >= ctx.nodes.size())
                    continue;
                is_assign_target[tgt] = 1;
                if (ctx.nodes[tgt].type == NodeType::Identifier)
                    reassigned[owner_of_node(state, tgt)].insert(
                        std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length));
            }
        } else if (n.type == NodeType::LocalDecl) {
            for (uint32_t ii = 0; ii < n.as.local_decl.ident_count; ++ii) {
                uint32_t idi = ctx.block_statements[n.as.local_decl.first_ident + ii];
                if (idi < ctx.nodes.size() && ctx.nodes[idi].type == NodeType::Identifier) {
                    is_decl_ident[idi] = 1;
                    decl_counts[{ owner_of_node(state, i),
                        std::string_view(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length) }]++;
                }
            }
        } else if (n.type == NodeType::FunctionDef) {
            for (uint32_t pi = 0; pi < n.as.func_def.param_count; ++pi) {
                uint32_t pid = ctx.block_statements[n.as.func_def.first_param + pi];
                if (pid < ctx.nodes.size() && ctx.nodes[pid].type == NodeType::Identifier)
                    is_decl_ident[pid] = 1;
            }
            if (n.as.func_def.named_vararg_ident != 0xFFFFFFFF)
                is_decl_ident[n.as.func_def.named_vararg_ident] = 1;
        } else if (n.type == NodeType::ForStatement) {
            if (n.as.for_stmt.var_ident != 0xFFFFFFFF)
                is_decl_ident[n.as.for_stmt.var_ident] = 1;
        } else if (n.type == NodeType::GenericForStatement) {
            for (uint32_t vi = 0; vi < n.as.generic_for.var_count; ++vi) {
                uint32_t vid = ctx.block_statements[n.as.generic_for.first_var + vi];
                if (vid < ctx.nodes.size())
                    is_decl_ident[vid] = 1;
            }
        }
    }

    //------------------ identifier nodes indexed by name for the usage scan
    std::unordered_map<std::string_view, std::vector<uint32_t>> idents_by_name;
    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        if (ctx.nodes[i].type == NodeType::Identifier)
            idents_by_name[std::string_view(ctx.nodes[i].as.ident.name, ctx.nodes[i].as.ident.length)].push_back(i);
    }

    //------------------ qualification A-D over every `local p = {..}` declaration
    std::vector<StructCandidate> cands;
    for (uint32_t di = 0; di < ctx.nodes.size(); ++di) {
        const auto &nd = ctx.nodes[di];
        if (nd.type != NodeType::LocalDecl)
            continue;
        if (nd.as.local_decl.ident_count != 1 || nd.as.local_decl.value_count != 1)
            continue;
        uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident];
        uint32_t vi = ctx.block_statements[nd.as.local_decl.first_value];
        if (idi >= ctx.nodes.size() || ctx.nodes[idi].type != NodeType::Identifier)
            continue;
        const auto &idn = ctx.nodes[idi];
        if (idn.as.ident.is_global || idn.as.ident.is_captured)
            continue;
        if (vi >= ctx.nodes.size() || ctx.nodes[vi].type != NodeType::TableConstructor)
            continue;
        uint32_t owner = owner_of_node(state, di);
        std::string_view nm(idn.as.ident.name, idn.as.ident.length);
        if (state.goto_crossed_locals.count(nm))
            continue;
        auto dcit = decl_counts.find({ owner, nm });
        if (dcit == decl_counts.end() || dcit->second != 1)
            continue;
        auto rit = reassigned.find(owner);
        if (rit != reassigned.end() && rit->second.count(nm))
            continue;

        //------------------ A: only distinct string-literal keys, no positional items
        const auto &tc = ctx.nodes[vi].as.table_cons;
        StructCandidate c;
        c.owner = owner;
        c.name = std::string(nm);
        c.decl_stmt = di;
        c.decl_ident = idi;
        bool ok = true;
        std::set<std::string_view> ctor_seen;
        std::vector<std::string_view> ctor_names;
        for (uint32_t k = 0; k < tc.count && ok; ++k) {
            uint32_t ki = ctx.block_statements[tc.first_item + k * 2];
            uint32_t val = ctx.block_statements[tc.first_item + k * 2 + 1];
            if (ki == 0xFFFFFFFF || ki >= ctx.nodes.size() || ctx.nodes[ki].type != NodeType::String) {
                ok = false;
                break;
            }
            std::string_view fn(ctx.nodes[ki].as.string.text, ctx.nodes[ki].as.string.length);
            if (!ctor_seen.insert(fn).second) {
                ok = false;
                break;
            }
            ctor_names.push_back(fn);
            c.ctor_values.push_back(val);
        }
        if (!ok)
            continue;

        //------------------ C: every use is a literal-key `p.f` read or `p.f = ...` write target
        std::vector<StructCandidate::Write> writes;
        std::vector<uint32_t> reads;
        std::vector<std::string> read_fields;
        auto uit = idents_by_name.find(nm);
        if (uit != idents_by_name.end()) {
            for (uint32_t u : uit->second) {
                if (u == idi || is_decl_ident[u] || owner_of_node(state, u) != owner)
                    continue;
                //------------------ a use inside the ctor initializer reads an outer variable of the same name
                for (uint32_t cur = u; cur != 0xFFFFFFFF && cur != di; cur = parent[cur]) {
                    if (cur == vi) {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    break;
                if (parent[u] >= ctx.nodes.size()) {
                    ok = false;
                    break;
                }
                uint32_t par = parent[u];
                const auto &pn = ctx.nodes[par];
                if (pn.type != NodeType::TableAccess || pn.as.table_access.table != u) {
                    ok = false;
                    break;
                }
                uint32_t key = pn.as.table_access.key;
                if (key >= ctx.nodes.size() || ctx.nodes[key].type != NodeType::String) {
                    ok = false;
                    break;
                }
                std::string_view fn(ctx.nodes[key].as.string.text, ctx.nodes[key].as.string.length);
                if (is_assign_target[par]) {
                    uint32_t an = parent[par];
                    if (an >= ctx.nodes.size() || ctx.nodes[an].type != NodeType::Assignment) {
                        ok = false;
                        break;
                    }
                    const auto &asg = ctx.nodes[an].as.assign;
                    uint32_t val = 0xFFFFFFFF;
                    for (uint32_t ti = 0; ti < asg.target_count; ++ti) {
                        if (ctx.block_statements[asg.first_target + ti] == par) {
                            val = (ti < asg.value_count) ? ctx.block_statements[asg.first_value + ti] : 0xFFFFFFFF;
                            break;
                        }
                    }
                    writes.push_back({ par, val, std::string(fn) });
                } else {
                    reads.push_back(par);
                    read_fields.push_back(std::string(fn));
                }
            }
        }
        if (!ok)
            continue;
        if (reads.empty() && writes.empty())
            continue;

        //------------------ source order for writes (node index) and the full ordered field set
        for (size_t i = 1; i < writes.size(); ++i) {
            auto w = writes[i];
            size_t j = i;
            while (j > 0 && writes[j - 1].access > w.access) {
                writes[j] = writes[j - 1];
                --j;
            }
            writes[j] = w;
        }
        c.writes = std::move(writes);
        c.read_nodes = std::move(reads);
        c.read_fields = std::move(read_fields);

        std::vector<std::string_view> all(ctor_names);
        std::vector<bool> late_all(ctor_names.size(), false);
        std::set<std::string_view> late_seen;
        for (auto &w : c.writes) {
            if (ctor_seen.count(w.field))
                continue;
            if (!late_seen.insert(w.field).second)
                continue;
            all.push_back(w.field);
            late_all.push_back(true);
        }
        std::set<std::string_view> all_seen;
        for (auto f : all) {
            if (!all_seen.insert(f).second) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;

        //------------------ phantom reads: a read of a field never constructed or written would hit the generic table path; reject
        for (auto &rf : c.read_fields) {
            if (!all_seen.count(rf)) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;

        //------------------ member-name validity and has_f prefix collisions
        bool members_ok = true;
        std::set<std::string> members;
        for (size_t i = 0; i < all.size(); ++i) {
            if (!valid_member_name(all[i])) {
                members_ok = false;
                break;
            }
            if (!members.insert(std::string(all[i])).second) {
                members_ok = false;
                break;
            }
            if (late_all[i] && !members.insert("has_" + std::string(all[i])).second) {
                members_ok = false;
                break;
            }
        }
        if (!members_ok)
            continue;

        c.fields.reserve(all.size());
        for (auto f : all)
            c.fields.emplace_back(f);
        c.late = std::move(late_all);
        cands.push_back(std::move(c));
    }
    if (cands.empty())
        return;

    //------------------ late-field read presence: a write earlier in the same or an enclosing block proves it
    auto find_stmt_index = [&](uint32_t block, uint32_t stmt) -> uint32_t {
        const auto &b = ctx.nodes[block].as.block;
        for (uint32_t i = 0; i < b.count; ++i) {
            if (ctx.block_statements[b.first_statement + i] == stmt)
                return i;
        }
        return 0xFFFFFFFFu;
    };
    auto block_writes_field = [&](uint32_t block, uint32_t before, uint32_t owner, std::string_view nm,
        std::string_view fld) -> bool {
        const auto &b = ctx.nodes[block].as.block;
        for (uint32_t i = 0; i < before && i < b.count; ++i) {
            uint32_t st = ctx.block_statements[b.first_statement + i];
            if (st >= ctx.nodes.size() || ctx.nodes[st].type != NodeType::Assignment)
                continue;
            if (owner_of_node(state, st) != owner)
                continue;
            const auto &asg = ctx.nodes[st].as.assign;
            for (uint32_t ti = 0; ti < asg.target_count; ++ti) {
                uint32_t tg = ctx.block_statements[asg.first_target + ti];
                if (tg >= ctx.nodes.size() || ctx.nodes[tg].type != NodeType::TableAccess)
                    continue;
                const auto &ta = ctx.nodes[tg].as.table_access;
                if (ta.table >= ctx.nodes.size() || ctx.nodes[ta.table].type != NodeType::Identifier)
                    continue;
                if (ta.key >= ctx.nodes.size() || ctx.nodes[ta.key].type != NodeType::String)
                    continue;
                if (std::string_view(ctx.nodes[ta.table].as.ident.name, ctx.nodes[ta.table].as.ident.length) != nm)
                    continue;
                if (std::string_view(ctx.nodes[ta.key].as.string.text, ctx.nodes[ta.key].as.string.length) != fld)
                    continue;
                return true;
            }
        }
        return false;
    };
    auto block_has_jump = [&](uint32_t block) -> bool {
        const auto &b = ctx.nodes[block].as.block;
        for (uint32_t i = 0; i < b.count; ++i) {
            uint32_t st = ctx.block_statements[b.first_statement + i];
            if (st < ctx.nodes.size()
                && (ctx.nodes[st].type == NodeType::GotoStatement || ctx.nodes[st].type == NodeType::LabelStatement))
                return true;
        }
        return false;
    };
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        StructCandidate &c = cands[ci];
        for (size_t ri = 0; ri < c.read_nodes.size(); ++ri) {
            std::string_view fld(c.read_fields[ri]);
            size_t fi = 0xFFFFFFFFu;
            for (size_t i = 0; i < c.fields.size(); ++i) {
                if (c.fields[i] == fld) {
                    fi = i;
                    break;
                }
            }
            if (fi == 0xFFFFFFFFu || !c.late[fi])
                continue;
            bool proven = false;
            uint32_t cur = c.read_nodes[ri];
            while (!proven && cur != 0xFFFFFFFF && cur < ctx.nodes.size()) {
                uint32_t p = parent[cur];
                if (p != 0xFFFFFFFF && p < ctx.nodes.size() && ctx.nodes[p].type == NodeType::Block) {
                    //------------------ a goto/label in a block on the path can jump over the write
                    if (block_has_jump(p))
                        break;
                    uint32_t si = find_stmt_index(p, cur);
                    if (si != 0xFFFFFFFFu && block_writes_field(p, si, c.owner, c.name, fld))
                        proven = true;
                }
                cur = p;
            }
            if (proven)
                state.struct_proven_reads.insert(c.read_nodes[ri]);
        }
    }

    //------------------ optimistic insert of every candidate; kinds commit monotonically (Unknown -> known only)
    for (auto &c : cands) {
        StructLocal sl;
        sl.fields = c.fields;
        sl.kinds.assign(c.fields.size(), FieldKind::Unknown);
        sl.late = c.late;
        sl.decl_node = c.decl_stmt;
        state.struct_locals[{ c.owner, c.name }] = std::move(sl);
    }

    auto field_kind = [&](uint32_t expr) -> FieldKind {
        if (expr == 0xFFFFFFFF || expr >= ctx.nodes.size())
            return FieldKind::Unknown;
        const auto &en = ctx.nodes[expr];
        if (en.type == NodeType::String)
            return FieldKind::Str;
        if (is_integer_typed_expr(ctx, state, expr))
            return FieldKind::Int;
        if (yields_number(ctx, state, expr, &sk.known_numbers))
            return FieldKind::Double;
        return FieldKind::Unknown;
    };
    auto reject = [&](StructCandidate &c) {
        state.struct_locals.erase({ c.owner, c.name });
    };
    auto find_field = [](const StructCandidate &c, std::string_view f) -> size_t {
        for (size_t i = 0; i < c.fields.size(); ++i) {
            if (c.fields[i] == f)
                return i;
        }
        return 0xFFFFFFFFu;
    };

    std::vector<uint8_t> alive(cands.size(), 1);
    size_t cap = cands.size() * 4 + 8;

    //------------------ Phase A: commit ctor seed kinds (iterate: ctor values may read other candidates' fields)
    size_t iter = 0;
    bool changed = true;
    while (changed && iter++ < cap) {
        changed = false;
        for (size_t ci = 0; ci < cands.size(); ++ci) {
            if (!alive[ci])
                continue;
            StructCandidate &c = cands[ci];
            auto sit = state.struct_locals.find({ c.owner, c.name });
            if (sit == state.struct_locals.end()) {
                alive[ci] = 0;
                continue;
            }
            std::vector<FieldKind> &kinds = sit->second.kinds;
            for (size_t i = 0; i < c.ctor_values.size(); ++i) {
                FieldKind k = field_kind(c.ctor_values[i]);
                if (k == FieldKind::Unknown)
                    continue;
                if (kinds[i] != k) {
                    kinds[i] = k;
                    changed = true;
                }
            }
        }
    }
    //------------------ ctor seeds that never resolved are permanently unknown -> reject
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        if (!alive[ci])
            continue;
        StructCandidate &c = cands[ci];
        auto sit = state.struct_locals.find({ c.owner, c.name });
        if (sit == state.struct_locals.end()) {
            alive[ci] = 0;
            continue;
        }
        bool unresolved = false;
        for (size_t i = 0; i < c.ctor_values.size(); ++i) {
            if (sit->second.kinds[i] == FieldKind::Unknown) {
                unresolved = true;
                break;
            }
        }
        if (unresolved) {
            reject(c);
            alive[ci] = 0;
        }
    }

    //------------------ Phase B: validate writes; a still-Unknown value is deferred, only stable mismatches reject
    std::vector<uint8_t> pending(cands.size(), 0);
    iter = 0;
    changed = true;
    while (changed && iter++ < cap) {
        changed = false;
        for (size_t ci = 0; ci < cands.size(); ++ci) {
            if (!alive[ci])
                continue;
            StructCandidate &c = cands[ci];
            auto sit = state.struct_locals.find({ c.owner, c.name });
            if (sit == state.struct_locals.end()) {
                alive[ci] = 0;
                pending[ci] = 0;
                continue;
            }
            std::vector<FieldKind> &kinds = sit->second.kinds;
            bool bad = false;
            bool deferred = false;
            for (auto &w : c.writes) {
                size_t fi = find_field(c, w.field);
                if (fi == 0xFFFFFFFFu) {
                    bad = true;
                    break;
                }
                FieldKind wk = field_kind(w.value);
                if (wk == FieldKind::Unknown) {
                    deferred = true;
                    continue;
                }
                if (kinds[fi] == FieldKind::Unknown) {
                    kinds[fi] = wk;
                    changed = true;
                } else if (kinds[fi] != wk) {
                    bad = true;
                    break;
                }
            }
            if (bad) {
                reject(c);
                alive[ci] = 0;
                pending[ci] = 0;
                changed = true;
                continue;
            }
            if (pending[ci] && !deferred)
                changed = true;
            pending[ci] = deferred ? 1 : 0;
        }
    }
    //------------------ write values that never resolved (or iteration cap) -> reject everything still pending
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        if (alive[ci] && pending[ci]) {
            reject(cands[ci]);
            alive[ci] = 0;
        }
    }

    //------------------ re-validate against the final state: a kind may come from a candidate rejected afterwards
    iter = 0;
    changed = true;
    while (changed && iter++ < cap) {
        changed = false;
        for (size_t ci = 0; ci < cands.size(); ++ci) {
            if (!alive[ci])
                continue;
            StructCandidate &c = cands[ci];
            auto sit = state.struct_locals.find({ c.owner, c.name });
            if (sit == state.struct_locals.end()) {
                alive[ci] = 0;
                continue;
            }
            std::vector<FieldKind> &kinds = sit->second.kinds;
            bool bad = false;
            for (size_t i = 0; i < c.ctor_values.size() && !bad; ++i) {
                if (field_kind(c.ctor_values[i]) != kinds[i])
                    bad = true;
            }
            for (auto &w : c.writes) {
                if (bad)
                    break;
                size_t fi = find_field(c, w.field);
                if (fi == 0xFFFFFFFFu || field_kind(w.value) == FieldKind::Unknown
                    || field_kind(w.value) != kinds[fi]) {
                    bad = true;
                    break;
                }
            }
            if (bad) {
                reject(c);
                alive[ci] = 0;
                changed = true;
            }
        }
    }

    //------------------ shape registration: one C++ struct type per distinct field/kind/late layout
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        if (!alive[ci])
            continue;
        StructCandidate &c = cands[ci];
        StructLocal &sl = state.struct_locals[{ c.owner, c.name }];
        std::string key = struct_shape_key(sl);
        if (state.struct_shapes.count(key))
            continue;
        StructShape ss;
        ss.fields = sl.fields;
        ss.kinds = sl.kinds;
        ss.late = sl.late;
        std::string tn = "S_";
        for (size_t i = 0; i < sl.fields.size(); ++i) {
            if (i)
                tn += "_";
            tn += sl.fields[i];
            tn += "_";
            tn += (sl.kinds[i] == FieldKind::Int ? 'i' : (sl.kinds[i] == FieldKind::Double ? 'd' : 's'));
            if (sl.late[i])
                tn += "_h";
        }
        bool dup = true;
        while (dup) {
            dup = false;
            for (auto &kv : state.struct_shapes) {
                if (kv.second.name == tn) {
                    tn += "_r";
                    dup = true;
                    break;
                }
            }
        }
        ss.name = tn;
        state.struct_shapes[key] = std::move(ss);
    }

    //------------------ promoted locals must not be hoisted or type-skipped as plain tables by codegen
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        if (alive[ci]) {
            state.table_typed_locals.erase(cands[ci].name);
        }
    }
}

}
