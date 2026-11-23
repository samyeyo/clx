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

//------------------ same_expr: structural equality of two expression subtrees, used to prove a swap's key pairing
static bool same_expr(const ASTContext &ctx, uint32_t a, uint32_t b) {
    if (a == b)
        return true;
    if (a >= ctx.nodes.size() || b >= ctx.nodes.size())
        return false;
    const ASTNode &x = ctx.nodes[a];
    const ASTNode &y = ctx.nodes[b];
    if (x.type != y.type)
        return false;
    switch (x.type) {
    case NodeType::Identifier:
        return std::string_view(x.as.ident.name, x.as.ident.length)
            == std::string_view(y.as.ident.name, y.as.ident.length);
    case NodeType::Number:
        return x.as.number.val == y.as.number.val;
    case NodeType::Integer:
        return x.as.integer.val == y.as.integer.val;
    case NodeType::BinaryOp:
        return x.as.bin_op.op == y.as.bin_op.op && same_expr(ctx, x.as.bin_op.left, y.as.bin_op.left)
            && same_expr(ctx, x.as.bin_op.right, y.as.bin_op.right);
    case NodeType::UnaryOp:
        return x.as.unary_op.op == y.as.unary_op.op && same_expr(ctx, x.as.unary_op.expr, y.as.unary_op.expr);
    case NodeType::ParenExpression:
        return same_expr(ctx, x.as.paren_expr.expr, y.as.paren_expr.expr);
    default:
        return false;
    }
}

//------------------ try_match_swap_idiom: true when pos..pos+2 of a block swap two slots of one hoisted table
bool CodeEmitter::try_match_swap_idiom(uint32_t block_idx, uint32_t pos, SwapIdiomMatch &m) {
    if (block_idx >= ctx.nodes.size() || ctx.nodes[block_idx].type != NodeType::Block)
        return false;
    const ASTNode &blk = ctx.nodes[block_idx];
    if (pos + 2 >= blk.as.block.count)
        return false;
    uint32_t a = ctx.block_statements[blk.as.block.first_statement + pos];
    uint32_t b = ctx.block_statements[blk.as.block.first_statement + pos + 1];
    uint32_t c = ctx.block_statements[blk.as.block.first_statement + pos + 2];
    if (a >= ctx.nodes.size() || b >= ctx.nodes.size() || c >= ctx.nodes.size())
        return false;
    const ASTNode &na = ctx.nodes[a];
    const ASTNode &nb = ctx.nodes[b];
    const ASTNode &nc = ctx.nodes[c];
    //------------------ shape: `local x = A[i]` then `A[j] = A[k]` then `A[l] = x`
    if (na.type != NodeType::LocalDecl || nb.type != NodeType::Assignment || nc.type != NodeType::Assignment)
        return false;
    if (state.dead_stmts.count(a))
        return false;
    if (na.as.local_decl.ident_count != 1 || na.as.local_decl.value_count != 1)
        return false;
    if (nb.as.assign.target_count != 1 || nb.as.assign.value_count != 1)
        return false;
    if (nc.as.assign.target_count != 1 || nc.as.assign.value_count != 1)
        return false;
    uint32_t temp_id = ctx.block_statements[na.as.local_decl.first_ident];
    uint32_t va = ctx.block_statements[na.as.local_decl.first_value];
    uint32_t tb = ctx.block_statements[nb.as.assign.first_target];
    uint32_t vb = ctx.block_statements[nb.as.assign.first_value];
    uint32_t tc = ctx.block_statements[nc.as.assign.first_target];
    uint32_t vc = ctx.block_statements[nc.as.assign.first_value];
    if (temp_id >= ctx.nodes.size() || va >= ctx.nodes.size() || tb >= ctx.nodes.size() || vb >= ctx.nodes.size()
        || tc >= ctx.nodes.size() || vc >= ctx.nodes.size())
        return false;
    if (ctx.nodes[temp_id].type != NodeType::Identifier)
        return false;
    if (ctx.nodes[temp_id].as.ident.is_captured || ctx.nodes[temp_id].as.ident.is_global)
        return false;
    if (ctx.nodes[va].type != NodeType::TableAccess || ctx.nodes[tb].type != NodeType::TableAccess
        || ctx.nodes[vb].type != NodeType::TableAccess || ctx.nodes[tc].type != NodeType::TableAccess)
        return false;
    if (ctx.nodes[vc].type != NodeType::Identifier || ctx.nodes[vc].as.ident.is_global)
        return false;
    std::string_view temp_name(ctx.nodes[temp_id].as.ident.name, ctx.nodes[temp_id].as.ident.length);
    if (temp_name != std::string_view(ctx.nodes[vc].as.ident.name, ctx.nodes[vc].as.ident.length))
        return false;
    //------------------ all four accesses must name the same hoisted table
    uint32_t tbls[4] = { ctx.nodes[va].as.table_access.table, ctx.nodes[tb].as.table_access.table,
        ctx.nodes[vb].as.table_access.table, ctx.nodes[tc].as.table_access.table };
    for (uint32_t ti : tbls) {
        if (ti >= ctx.nodes.size() || ctx.nodes[ti].type != NodeType::Identifier || ctx.nodes[ti].as.ident.is_global)
            return false;
    }
    std::string_view tn(ctx.nodes[tbls[0]].as.ident.name, ctx.nodes[tbls[0]].as.ident.length);
    for (uint32_t ti : tbls) {
        if (std::string_view(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length) != tn)
            return false;
    }
    if (hoisted_table_ptr(tbls[0]).empty())
        return false;
    uint32_t k_read = ctx.nodes[va].as.table_access.key;
    uint32_t k_tgt_b = ctx.nodes[tb].as.table_access.key;
    uint32_t k_val_b = ctx.nodes[vb].as.table_access.key;
    uint32_t k_tgt_c = ctx.nodes[tc].as.table_access.key;
    for (uint32_t k : { k_read, k_tgt_b, k_val_b, k_tgt_c }) {
        if (k >= ctx.nodes.size() || !is_purely_integer_expr(ctx, state, k))
            return false;
    }
    //------------------ the stores must target exactly the two slots that were read, or it is not a swap
    if (!same_expr(ctx, k_read, k_tgt_b) || !same_expr(ctx, k_val_b, k_tgt_c))
        return false;
    //------------------ the temp must not be referenced anywhere else in this block subtree
    if (count_ident_uses(ctx, block_idx, temp_name, temp_id, vc) != 0)
        return false;
    m.temp_ident = temp_id;
    m.table_ident = tbls[0];
    m.read_a = va;
    m.read_b = vb;
    m.key_a = k_read;
    m.key_b = k_val_b;
    return true;
}

//------------------ emit_swap_idiom: raw two-slot exchange on a hoisted array, generic stores when out of the array part
void CodeEmitter::emit_swap_idiom(const SwapIdiomMatch &m, uint32_t tag) {
    const std::string s = std::to_string(tag);
    out << "{ clx::LTable* _sw" << s << " = " << hoisted_table_ptr(m.table_ident) << "; int64_t _sa" << s
        << " = static_cast<int64_t>(";
    emit_native(m.key_a);
    out << "); int64_t _sb" << s << " = static_cast<int64_t>(";
    emit_native(m.key_b);
    out << "); if (_sa" << s << " >= 1 && _sb" << s << " >= 1 && static_cast<size_t>(_sa" << s << ") <= _sw" << s
        << "->array_size && static_cast<size_t>(_sb" << s << ") <= _sw" << s
        << "->array_size && static_cast<size_t>(_sa" << s << ") <= _sw" << s << "->array_cap && static_cast<size_t>(_sb"
        << s << ") <= _sw" << s << "->array_cap) [[likely]] {"
        << " clx::TValue _v1" << s << " = _sw" << s << "->array[_sa" << s << " - 1];"
        << " clx::ValueType _y1" << s << " = _sw" << s << "->array_types[_sa" << s << " - 1];"
        << " clx::TValue _v2" << s << " = _sw" << s << "->array[_sb" << s << " - 1];"
        << " clx::ValueType _y2" << s << " = _sw" << s << "->array_types[_sb" << s << " - 1];"
        << " clx::gc_barrier_table(L, _sw" << s << ", _v2" << s << ", _y2" << s << ");"
        << " clx::gc_barrier_table(L, _sw" << s << ", _v1" << s << ", _y1" << s << ");"
        << " _sw" << s << "->array[_sa" << s << " - 1] = _v2" << s << ";"
        << " _sw" << s << "->array_types[_sa" << s << " - 1] = _y2" << s << ";"
        << " _sw" << s << "->array[_sb" << s << " - 1] = _v1" << s << ";"
        << " _sw" << s << "->array_types[_sb" << s << " - 1] = _y1" << s << ";"
        << " clx::table_hint_on_array_write(_sw" << s << ", static_cast<size_t>(_sa" << s << " - 1), _y2" << s
        << "); clx::table_hint_on_array_write(_sw" << s << ", static_cast<size_t>(_sb" << s << " - 1), _y1"
        << s << "); } else {"
        << " clx::LValue _f1" << s << " = ";
    emit_node(m.read_a);
    out << "; clx::LValue _f2" << s << " = ";
    emit_node(m.read_b);
    out << "; clx::LValue _tb" << s << " = clx::LValue(clx::ValueType::Table, _sw" << s << ");"
        << " clx::table_set(L, _tb" << s << ", clx::LValue(static_cast<int64_t>(_sa" << s << ")), _f2" << s << ");"
        << " clx::table_set(L, _tb" << s << ", clx::LValue(static_cast<int64_t>(_sb" << s << ")), _f1" << s << ");"
        << " } }\n";
}

//------------------ try_match_cmp_swap: `if (A[i] OP A[j]) swap` on one hoisted table, same two integer slots on both sides
bool CodeEmitter::try_match_cmp_swap(uint32_t if_idx, CmpSwapMatch &m) {
    if (if_idx >= ctx.nodes.size() || ctx.nodes[if_idx].type != NodeType::IfStatement)
        return false;
    const ASTNode &in = ctx.nodes[if_idx];
    uint32_t cond = in.as.if_stmt.condition;
    while (cond != 0xFFFFFFFF && cond < ctx.nodes.size() && ctx.nodes[cond].type == NodeType::ParenExpression)
        cond = ctx.nodes[cond].as.paren_expr.expr;
    if (cond == 0xFFFFFFFF || cond >= ctx.nodes.size() || ctx.nodes[cond].type != NodeType::BinaryOp)
        return false;
    const ASTNode &bc = ctx.nodes[cond];
    int op = bc.as.bin_op.op;
    bool is_lt = op == static_cast<int>(BinaryOp::Lt) || op == static_cast<int>(BinaryOp::Gt);
    bool is_le = op == static_cast<int>(BinaryOp::Le) || op == static_cast<int>(BinaryOp::Ge);
    if (!is_lt && !is_le)
        return false;
    bool swapped = op == static_cast<int>(BinaryOp::Gt) || op == static_cast<int>(BinaryOp::Ge);
    uint32_t ord_l = swapped ? bc.as.bin_op.right : bc.as.bin_op.left;
    uint32_t ord_r = swapped ? bc.as.bin_op.left : bc.as.bin_op.right;
    if (ord_l >= ctx.nodes.size() || ord_r >= ctx.nodes.size())
        return false;
    const ASTNode &ln = ctx.nodes[ord_l];
    const ASTNode &rn = ctx.nodes[ord_r];
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
    if (ctx.nodes[lb].as.ident.is_global || ctx.nodes[rb].as.ident.is_global)
        return false;
    std::string_view ltn(ctx.nodes[lb].as.ident.name, ctx.nodes[lb].as.ident.length);
    std::string_view rtn(ctx.nodes[rb].as.ident.name, ctx.nodes[rb].as.ident.length);
    if (ltn != rtn)
        return false;
    if (!is_purely_integer_expr(ctx, state, lk) || !is_purely_integer_expr(ctx, state, rk))
        return false;
    if (hoisted_table_ptr(lb).empty())
        return false;
    uint32_t tb = in.as.if_stmt.then_block;
    if (tb == 0xFFFFFFFF || tb >= ctx.nodes.size() || ctx.nodes[tb].type != NodeType::Block)
        return false;
    if (ctx.nodes[tb].as.block.count != 3)
        return false;
    SwapIdiomMatch swm;
    if (!try_match_swap_idiom(tb, 0, swm))
        return false;
    uint32_t st = swm.table_ident;
    if (st >= ctx.nodes.size() || ctx.nodes[st].type != NodeType::Identifier)
        return false;
    std::string_view stn(ctx.nodes[st].as.ident.name, ctx.nodes[st].as.ident.length);
    if (stn != ltn)
        return false;
    bool straight = same_expr(ctx, lk, swm.key_a) && same_expr(ctx, rk, swm.key_b);
    bool crossed = same_expr(ctx, lk, swm.key_b) && same_expr(ctx, rk, swm.key_a);
    if (!straight && !crossed)
        return false;
    m.table_ident = lb;
    m.ord_l = ord_l;
    m.ord_r = ord_r;
    m.key_ol = lk;
    m.key_or = rk;
    m.cmp_op = is_lt ? static_cast<int>(BinaryOp::Lt) : static_cast<int>(BinaryOp::Le);
    m.cond_line = ctx.nodes[cond].line;
    m.body_line = ctx.nodes[tb].line;
    return true;
}

//------------------ emit_cmp_swap: one bounds/tag proof feeding a native compare and, when true, a raw two-slot exchange
void CodeEmitter::emit_cmp_swap(const CmpSwapMatch &m, uint32_t tag) {
    const std::string s = std::to_string(tag);
    const char *cmp = (m.cmp_op == static_cast<int>(BinaryOp::Le)) ? "<=" : "<";
    const char *fn = (m.cmp_op == static_cast<int>(BinaryOp::Le)) ? "le" : "lt";
    out << "#line " << m.cond_line << " \"" << ctx.filename << "\"\n";
    out << "{ clx::LTable* _cs" << s << " = " << hoisted_table_ptr(m.table_ident) << ";\n";
    out << "int64_t _cko1" << s << " = static_cast<int64_t>(";
    emit_native(m.key_ol);
    out << ");\nint64_t _cko2" << s << " = static_cast<int64_t>(";
    emit_native(m.key_or);
    out << ");\n";
    out << "auto _csarr" << s << " = _cs" << s << "->array;\n";
    out << "auto _cstyp" << s << " = _cs" << s << "->array_types;\n";
    out << "auto _cssz" << s << " = _cs" << s << "->array_size;\n";
    out << "auto _cscap" << s << " = _cs" << s << "->array_cap;\n";
    out << "bool _csgen" << s << " = true;\n";
    out << "if (_cko1" << s << " >= 1 && _cko2" << s << " >= 1 && static_cast<size_t>(_cko1" << s << ") <= _cssz" << s
        << " && static_cast<size_t>(_cko2" << s << ") <= _cssz" << s << " && static_cast<size_t>(_cko1" << s
        << ") <= _cscap" << s << " && static_cast<size_t>(_cko2" << s << ") <= _cscap" << s << ") [[likely]] {\n";
    out << "#line " << m.body_line << " \"" << ctx.filename << "\"\n";
    out << "clx::TValue _csv1" << s << " = _csarr" << s << "[_cko1" << s << " - 1];\n";
    out << "clx::ValueType _csy1" << s << " = _cstyp" << s << "[_cko1" << s << " - 1];\n";
    out << "clx::TValue _csv2" << s << " = _csarr" << s << "[_cko2" << s << " - 1];\n";
    out << "clx::ValueType _csy2" << s << " = _cstyp" << s << "[_cko2" << s << " - 1];\n";
    out << "if (_csy1" << s << " == clx::ValueType::Int64 && _csy2" << s << " == clx::ValueType::Int64) [[likely]] {\n";
    out << "if (_csv1" << s << ".payload.i64 " << cmp << " _csv2" << s << ".payload.i64) {\n";
    out << "int64_t _cst" << s << " = _csv1" << s << ".payload.i64;\n";
    out << "_csarr" << s << "[_cko1" << s << " - 1].payload.i64 = _csv2" << s << ".payload.i64;\n";
    out << "_csarr" << s << "[_cko2" << s << " - 1].payload.i64 = _cst" << s << ";\n";
    out << "}\n_csgen" << s << " = false;\n";
    out << "} else if ((_csy1" << s << " == clx::ValueType::Int64 || _csy1" << s << " == clx::ValueType::Double)"
        << " && (_csy2" << s << " == clx::ValueType::Int64 || _csy2" << s << " == clx::ValueType::Double)) {\n";
    out << "double _csd1" << s << " = (_csy1" << s << " == clx::ValueType::Int64"
        << " ? static_cast<double>(_csv1" << s << ".payload.i64) : _csv1" << s << ".payload.f64);\n";
    out << "double _csd2" << s << " = (_csy2" << s << " == clx::ValueType::Int64"
        << " ? static_cast<double>(_csv2" << s << ".payload.i64) : _csv2" << s << ".payload.f64);\n";
    out << "if (_csd1" << s << " " << cmp << " _csd2" << s << ") {\n";
    out << "_csarr" << s << "[_cko1" << s << " - 1] = _csv2" << s << ";\n";
    out << "_cstyp" << s << "[_cko1" << s << " - 1] = _csy2" << s << ";\n";
    out << "_csarr" << s << "[_cko2" << s << " - 1] = _csv1" << s << ";\n";
    out << "_cstyp" << s << "[_cko2" << s << " - 1] = _csy1" << s << ";\n";
    out << "clx::gc_barrier_table(L, _cs" << s << ", _csarr" << s << "[_cko1" << s << " - 1], _cstyp" << s << "[_cko1"
        << s << " - 1]);\n";
    out << "clx::gc_barrier_table(L, _cs" << s << ", _csarr" << s << "[_cko2" << s << " - 1], _cstyp" << s << "[_cko2"
        << s << " - 1]);\n";
    out << "}\n_csgen" << s << " = false;\n";
    out << "}\n}\n";
    out << "if (_csgen" << s << ") {\n";
    out << "clx::LValue _cg1" << s << " = ";
    emit_node(m.ord_l);
    out << ";\nclx::LValue _cg2" << s << " = ";
    emit_node(m.ord_r);
    out << ";\nif (clx::" << fn << "(L, _cg1" << s << ", _cg2" << s << ").as_bool()) {\n";
    out << "clx::table_set(L, clx::LValue(clx::ValueType::Table, _cs" << s
        << "), clx::LValue(static_cast<int64_t>(_cko1" << s << ")), _cg2" << s << ");\n";
    out << "clx::table_set(L, clx::LValue(clx::ValueType::Table, _cs" << s
        << "), clx::LValue(static_cast<int64_t>(_cko2" << s << ")), _cg1" << s << ");\n";
    out << "}\n}\n}\n";
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
            //------------------ a temp consumed by swap fusion never exists, so it needs no scope guard
            SwapIdiomMatch _swm;
            if (try_match_swap_idiom(node_idx, i, _swm))
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
        out << "clx::FrameGuard _fg_block_" << node_idx << "(_fr);\n";
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
        std::unordered_set<uint32_t> jump_crossed;
        for (uint32_t i = 0; i < node.as.block.count; ++i) {
            uint32_t g_idx = ctx.block_statements[node.as.block.first_statement + i];
            if (ctx.nodes[g_idx].type != NodeType::GotoStatement)
                continue;
            auto gt = state.goto_targets.find(g_idx);
            if (gt == state.goto_targets.end())
                continue;
            for (uint32_t j = i + 1; j < node.as.block.count; ++j) {
                uint32_t t_idx = ctx.block_statements[node.as.block.first_statement + j];
                if (ctx.nodes[t_idx].type == NodeType::LabelStatement && t_idx == gt->second) {
                    for (uint32_t k = i + 1; k < j; ++k) {
                        uint32_t c_idx = ctx.block_statements[node.as.block.first_statement + k];
                        if (ctx.nodes[c_idx].type == NodeType::LocalDecl)
                            jump_crossed.insert(c_idx);
                    }
                    break;
                }
            }
        }
        bool after_goto = false;
        for (uint32_t i = 0; i < node.as.block.count; ++i) {
            uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
            const auto &st = ctx.nodes[stmt_idx];
            if (st.type == NodeType::GotoStatement)
                after_goto = true;
            if (st.type == NodeType::LabelStatement)
                after_goto = false;
            if ((after_goto || jump_crossed.count(stmt_idx)) && st.type == NodeType::LocalDecl) {
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
                        out << "_fr.push(*l_" << nm << ");\n";
                    } else if (in_native) {
                        out << "double l_" << nm << ";\n";
                        out << "bool _intf_l_" << nm << " = false;\n";
                        out << "int64_t _ii_l_" << nm << " = 0;\n";
                    } else {
                        out << "clx::LValue l_" << nm << ";\n";
                        out << "_fr.push(l_" << nm << ");\n";
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
            bool _pf = state.fast_body_funcs.count(_pn) > 0 && state.slow_call_funcs.count(_pn) == 0;
            if (_pf)
                state.fast_callables.insert(_pn);
            state.direct_callables.insert(_pn);
        }
    }

    size_t prev_locals = locals.size();
    int redecl_scopes = 0;
    std::set<std::string_view> current_block_vars;

    //------------------ a block with labels keeps its statements in an inner scope so a goto cannot bypass their initializers
    uint32_t body_end = node.as.block.count;
    bool has_label = false;
    for (uint32_t i = 0; i < node.as.block.count; ++i) {
        uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
        if (ctx.nodes[stmt_idx].type == NodeType::LabelStatement) {
            has_label = true;
            break;
        }
    }
    if (has_label)
        while (body_end > 0) {
            uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + body_end - 1];
            if (ctx.nodes[stmt_idx].type != NodeType::LabelStatement)
                break;
            --body_end;
        }
    if (has_label)
        out << "{\n";

    for (uint32_t i = 0; i < body_end; ++i) {
        uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
        if (state.dead_stmts.count(stmt_idx))
            continue;
        const auto &stmt = ctx.nodes[stmt_idx];

        //------------------ array swap fusion: `local x = A[i]; A[j] = A[k]; A[l] = x` becomes one raw exchange
        SwapIdiomMatch _sw;
        if (try_match_swap_idiom(node_idx, i, _sw)) {
            emit_swap_idiom(_sw, stmt_idx);
            i += 2;
            continue;
        }

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
        if (stmt.line > 0 && !state.in_fast_function) {
            std::string _ssite = emit_call_site(stmt.line, false, false);
            out << "L->current_site = " << _ssite << ";\n";
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

    if (has_label) {
        out << "}\n";
        for (uint32_t i = body_end; i < node.as.block.count; ++i) {
            uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
            if (state.dead_stmts.count(stmt_idx))
                continue;
            emit_node(stmt_idx);
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

//------------------ emit_fast_variant: emits the `_fast_<name>_impl` Y-combinator body and the `_fast_<name>` wrapper

void CodeEmitter::emit_fast_variant(std::string_view name, uint32_t v_idx) {
    const auto &fn = ctx.nodes[v_idx];
    out << "auto _fast_" << name << "_impl = ";
    bool saved_emit_fast = state.emit_fast_lambda;
    bool saved_in_fast = state.in_fast_function;
    std::string_view saved_fast_func = state.current_fast_func;
    state.emit_fast_lambda = true;
    state.in_fast_function = true;
    state.current_fast_func = name;
    uint32_t saved_arena_fast = state.current_arena_func;
    state.current_arena_func = 0xFFFFFFFF;
    emit_node(v_idx);
    state.current_arena_func = saved_arena_fast;
    state.emit_fast_lambda = saved_emit_fast;
    state.in_fast_function = saved_in_fast;
    state.current_fast_func = saved_fast_func;
    out << ";\n";
    out << "#line " << fn.line << " \"" << ctx.filename << "\"\n";
    out << "auto _fast_" << name << " = [=](clx::FrameBase& _fr";
    for (uint32_t a = 0; a < state.func_param_counts[name]; ++a)
        out << ", double p" << a;
    //------------------ fast call entry: raisable stack-overflow check, clearing the pending site a body left while unwinding
    out << ") -> double {\n";
    out << "    clx::LState *_L = _fr.owner;\n";
    out << "    _L->stack_overflow_site = nullptr;\n";
    out << "    double _ret = _fast_" << name << "_impl(_fast_" << name << "_impl, _fr";
    for (uint32_t a = 0; a < state.func_param_counts[name]; ++a)
        out << ", p" << a;
    out << ");\n";
    out << "    if (_L->stack_overflow_site != nullptr) [[unlikely]]\n";
    out << "        clx::raise_stack_overflow(_L);\n";
    out << "    return _ret;\n";
    out << "};\n";
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
    emit_dbl_impl_def(name, v_idx);
}

//------------------ emit_dbl_impl_def: triple-bound overload of a native-direct function, params from (double, flag, shadow)
void CodeEmitter::emit_dbl_impl_def(std::string_view name, uint32_t v_idx) {
    const auto &fn = ctx.nodes[v_idx];
    if (fn.type != NodeType::FunctionDef || fn.as.func_def.is_vararg)
        return;
    //------------------ void functions return stock zero values through the boxed impl, so no triple form is needed
    if (state.void_funcs.count(name) > 0)
        return;
    uint32_t nparams = fn.as.func_def.param_count;
    std::vector<std::string_view> seen(state.native_numbers.begin(), state.native_numbers.end());
    for (uint32_t i = 0; i < nparams; ++i) {
        uint32_t p_idx = ctx.block_statements[fn.as.func_def.first_param + i];
        if (p_idx >= ctx.nodes.size() || ctx.nodes[p_idx].type != NodeType::Identifier)
            return;
        std::string_view pname(ctx.nodes[p_idx].as.ident.name, ctx.nodes[p_idx].as.ident.length);
        if (ctx.nodes[p_idx].as.ident.is_captured)
            return;
        bool is_native_p = std::find(seen.begin(), seen.end(), pname) != seen.end()
            || state.param_numbers[v_idx].count(std::string(pname));
        if (!is_native_p)
            return;
        if (std::find(seen.begin(), seen.end(), pname) == seen.end())
            seen.push_back(pname);
    }

    out << "[[maybe_unused]] auto native_" << name << "_dbl_impl = [=](auto& self, clx::LState* L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", double u_d" << a << ", bool u_f" << a << ", int64_t u_s" << a;
    out << ") -> clx::LValue {\n";

    state.native_dbl_emitted.insert(name);
    auto saved_native_func = state.current_native_func;
    bool saved_in_native = state.in_native_impl;
    bool saved_in_dbl = state.in_dbl_impl;
    state.emit_native_impl = true;
    state.emit_dbl_impl = true;
    state.in_native_impl = true;
    state.in_dbl_impl = true;
    state.current_native_func = name;
    emit_node(v_idx);
    state.emit_native_impl = false;
    state.emit_dbl_impl = false;
    state.in_native_impl = saved_in_native;
    state.in_dbl_impl = saved_in_dbl;
    state.current_native_func = saved_native_func;
    out << ";\n";
    out << "#line " << fn.line << " \"" << ctx.filename << "\"\n";
    out << "[[maybe_unused]] auto native_" << name << "_dbl = [=](clx::LState* L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", double u_d" << a << ", bool u_f" << a << ", int64_t u_s" << a;
    out << ") -> clx::LValue { return native_" << name << "_dbl_impl(native_" << name << "_dbl_impl, L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", u_d" << a << ", u_f" << a << ", u_s" << a;
    out << "); };\n";
}

//------------------ emit_multi_impl_def: tuple-struct overload for a multi-double function; the closure is still emitted
void CodeEmitter::emit_multi_impl_def(std::string_view name, uint32_t v_idx, uint32_t tuple_n) {
    const auto &fn = ctx.nodes[v_idx];
    uint32_t nparams = fn.as.func_def.param_count;
    out << "auto native_" << name << " = [=](clx::LState* L";
    for (uint32_t a = 0; a < nparams; ++a)
        out << ", clx::LValue p" << a;
    out << ") -> ClxTup" << tuple_n << " {\n";

    state.multi_emitted.insert(name);
    auto saved_multi_func = state.current_multi_func;
    uint32_t saved_multi_count = state.current_multi_count;
    bool saved_in_multi = state.in_multi_impl;
    state.emit_multi_impl = true;
    state.in_multi_impl = true;
    state.current_multi_func = name;
    state.current_multi_count = tuple_n;
    emit_node(v_idx);
    state.emit_multi_impl = false;
    state.in_multi_impl = saved_in_multi;
    state.current_multi_func = saved_multi_func;
    state.current_multi_count = saved_multi_count;
    out << ";\n";
}

//------------------ emitFunctionDef: handles NodeType::FunctionDef
void CodeEmitter::emitFunctionDef(const ASTNode &node, uint32_t node_idx) {
    bool is_raw = state.emit_raw_lambda;
    bool is_fast = state.emit_fast_lambda;
    bool is_multi = state.emit_multi_impl;
    bool is_native = state.emit_native_impl || is_multi;
    //------------------ is_dbl: triple-bound overload body, params bound from (double, int-flag, int-shadow) triples
    bool is_dbl = state.emit_dbl_impl;
    //------------------ a nested closure returns MultiValue even inside a fast function, so the fast path is saved and restored
    bool saved_in_fast_function = state.in_fast_function;
    std::string_view saved_fast_func_name = state.current_fast_func;
    if (!is_fast) {
        state.in_fast_function = false;
        state.current_fast_func = "";
    }
    state.emit_raw_lambda = false;
    state.emit_fast_lambda = false;
    state.emit_native_impl = false;
    state.emit_multi_impl = false;
    state.emit_dbl_impl = false;
    bool prev_in_func = state.in_function_def;
    state.in_function_def = true;
    std::string prev_vararg_table = state.current_vararg_table;
    state.current_vararg_table.clear();

    auto saved_direct_callables = state.direct_callables;
    auto saved_fast_callables = state.fast_callables;
    auto saved_native_emitted = state.native_emitted;
    auto saved_native_dbl_emitted = state.native_dbl_emitted;
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
        //------------------ fast lambdas take the caller's frame: no LState of their own and a captured frame would dangle
        out << "[=](auto& self, clx::FrameBase& _fr";
        for (uint32_t i = 0; i < node.as.func_def.param_count; ++i) {
            out << ", double l_" << fast_param_cpp[i];
        }
        out << ") -> double {\n";
        //------------------ a fast self-call bypasses the wrappers' depth check; the guard returns to stay unwind-free
        out << "if (clx::stack_probe() < _fr.owner->stack_low) [[unlikely]]\n";
        out << "{\n";
        out << "    if (!_fr.owner->stack_overflow_site)\n";
        out << "        _fr.owner->stack_overflow_site = _fr.owner->current_site;\n";
        out << "    return 0.0;\n";
        out << "}\n";

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
        state.native_dbl_emitted = std::move(saved_native_dbl_emitted);
        state.hoisted_tables = std::move(saved_hoisted);
        state.in_native_impl = saved_in_native;
        state.current_native_func = saved_native_func;
        state.current_func_body = saved_func_body;
        state.in_fast_function = saved_in_fast_function;
        state.current_fast_func = saved_fast_func_name;
        out << "return 0.0;\n}";
        return;
    }

    if (is_native) {

        //------------------ native impls are entered lambda-to-lambda (self(self, ...)), bypassing the call wrappers' depth check
        out << "clx::stack_check(L);\n";
        //------------------ the impl is a lambda, not a closure, so globals need the function's own _ENV rather than the capture
        out << "clx::LValue _ENV = (L->current_func && L->current_func->env) ? clx::LValue(clx::ValueType::Table, "
               "L->current_func->env) : clx::LValue(clx::ValueType::Table, L->_G);\n";
        out << "clx::Frame<16> _fr(L);\n";
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
            bool force_generic = state.generic_body_func == node_idx;
            bool is_native_p = !force_generic
                && (std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                        != state.native_numbers.end()
                    || (!is_cap && state.param_numbers[node_idx].count(std::string(pname))));
            if (is_cap) {
                if (state.constant_upvalues.count(pname)) {
                    out << "clx::LValue l_" << cpp_name << " = p" << i << ";\n";
                    out << "_fr.push(l_" << cpp_name << ");\n";
                    if (!state.pure_numeric_arrays.count(pname)) {
                        out << "auto l_" << cpp_name << "_csnap = clx::make_upvalue(l_" << cpp_name << ");\n";
                        state.const_snapshot_cells.push_back(std::string(cpp_name));
                    }
                } else {
                    out << "clx::LUpValue l_" << cpp_name << " = clx::make_upvalue(p" << i << ");\n";
                    out << "_fr.push(*l_" << cpp_name << ");\n";
                }
            } else if (is_native_p) {
                if (is_dbl) {
                    out << "double l_" << cpp_name << " = u_d" << i << ";\n";
                    out << "bool _intf_l_" << cpp_name << " = u_f" << i << ";\n";
                    out << "int64_t _ii_l_" << cpp_name << " = u_s" << i << ";\n";
                } else {
                    out << "double l_" << cpp_name << " = clx::coerce_number(L, p" << i << ");\n";
                    out << "bool _intf_l_" << cpp_name << " = p" << i << ".type == clx::ValueType::Int64;\n";
                    out << "int64_t _ii_l_" << cpp_name << " = p" << i << ".as_integer();\n";
                }
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
        state.native_dbl_emitted = std::move(saved_native_dbl_emitted);
        state.hoisted_tables = std::move(saved_hoisted);
        state.current_func_body = saved_func_body;
        state.in_fast_function = saved_in_fast_function;
        state.current_fast_func = saved_fast_func_name;
        if (state.arena_table_sizes.count(node_idx))
            out << "clx::arena_reset(&_arena);\n";
        if (is_multi) {
            out << "return ClxTup" << state.current_multi_count << "{";
            for (uint32_t z = 0; z < state.current_multi_count; ++z)
                out << (z ? ", 0.0" : "0.0");
            out << "};\n}";
        } else {
            out << "return clx::LValue();\n}";
        }
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
    //------------------ _va_count/_va_args exist solely in vararg functions: skip the declarations otherwise
    if (node.as.func_def.is_vararg) {
        out << "size_t _va_count = (arg_count > " << node.as.func_def.param_count << ") ? (arg_count - "
            << node.as.func_def.param_count << ") : 0;\n";
        out << "const clx::LValue* _va_args = _va_count > 0 ? (args + " << node.as.func_def.param_count
            << ") : nullptr;\n";
    }

    uint32_t prev_func_idx = state.current_func_idx;
    state.current_func_idx = node_idx;
    out << "clx::Frame<16> _fr(L);\n";

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
        bool force_generic = state.generic_body_func == node_idx;
        bool is_native = !force_generic
            && (std::find(state.native_numbers.begin(), state.native_numbers.end(), pname)
                    != state.native_numbers.end()
                || (!is_cap && state.param_numbers[node_idx].count(std::string(pname))));

        if (is_cap) {
            if (state.constant_upvalues.count(pname)) {
                out << "clx::LValue l_" << cpp_name << " = (" << i << " < arg_count) ? args[" << i
                    << "] : clx::LValue();\n";
                out << "_fr.push(l_" << cpp_name << ");\n";
                if (!state.pure_numeric_arrays.count(pname)) {
                    out << "auto l_" << cpp_name << "_csnap = clx::make_upvalue(l_" << cpp_name << ");\n";
                    state.const_snapshot_cells.push_back(std::string(cpp_name));
                }
            } else {
                out << "clx::LUpValue l_" << cpp_name << ";\n";
                out << "l_" << cpp_name << " = clx::make_upvalue((" << i << " < arg_count) ? args[" << i
                    << "] : clx::LValue());\n";
                out << "_fr.push(*l_" << cpp_name << ");\n";
            }
        } else if (is_native) {
            //------------------ coerce_number keeps stock's arithmetic coercion (a numeric string is parsed, a non-number raises)
            out << "double l_" << cpp_name << " = (" << i << " < arg_count) ? clx::coerce_number(L, args[" << i
                << "]) : 0.0;\n";
            out << "bool _intf_l_" << cpp_name << " = (" << i << " < arg_count) && args[" << i
                << "].type == clx::ValueType::Int64;\n";
            out << "int64_t _ii_l_" << cpp_name << " = _intf_l_" << cpp_name << " ? args[" << i
                << "].val.payload.i64 : 0;\n";
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
            << "->settable(clx::LValue(L->intern_string(\"n\")), clx::integer(static_cast<int64_t>(_va_count)));\n";
        out << "for (size_t i = 0; i < _va_count; ++i) {\n";
        out << "    _t_" << vararg_cpp_name << "->settable(clx::integer(static_cast<int64_t>(i + 1)), _va_args[i]);\n";
        out << "}\n";
        out << "_fr.push(l_" << vararg_cpp_name << ");\n";
        state.current_vararg_table = vararg_cpp_name;
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
    state.current_vararg_table = prev_vararg_table;
    state.in_function_def = prev_in_func;
    state.in_native_impl = saved_in_native;
    state.current_native_func = saved_native_func;
    state.native_emitted = std::move(saved_native_emitted);
    state.native_dbl_emitted = std::move(saved_native_dbl_emitted);
    state.hoisted_tables = std::move(saved_hoisted);
    state.current_func_body = saved_func_body;
    state.current_func_idx = prev_func_idx;
    state.direct_callables = std::move(saved_direct_callables);
    state.fast_callables = std::move(saved_fast_callables);
    state.in_fast_function = saved_in_fast_function;
    state.current_fast_func = saved_fast_func_name;
    if (state.arena_table_sizes.count(node_idx))
        out << "clx::arena_reset(&_arena);\n";
    state.current_arena_func = saved_arena_func;
    out << "return clx::MultiValue();\n";
    out << "}";
    if (!is_raw) {
        out << ", static_cast<clx::LTable*>(" << env_cpp() << ".as_pointer())";
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

    //------------------ frames unlink by RAII at return, so the return expression is evaluated with the full root set live
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

    if (state.in_multi_impl) {
        if (state.current_arena_func != 0xFFFFFFFF)
            out << "clx::arena_reset(&_arena);\n";
        if (v_count == state.current_multi_count) {
            out << "return ClxTup" << state.current_multi_count << "{";
            for (uint32_t v = 0; v < v_count; ++v) {
                if (v)
                    out << ", ";
                out << "static_cast<double>(";
                emit_native(ctx.block_statements[first_v + v]);
                out << ")";
            }
            out << "};\n";
        } else if (v_count == 1) {
            out << "return ";
            uint32_t rv = ctx.block_statements[first_v];
            if (rv < ctx.nodes.size() && ctx.nodes[rv].type == NodeType::CallExpression) {
                const auto &ce = ctx.nodes[rv].as.call_expr;
                std::string_view callee(ctx.nodes[ce.target].as.ident.name, ctx.nodes[ce.target].as.ident.length);
                emit_multi_call(callee, ce.first_arg, ce.arg_count);
            } else {
                emit_native(rv);
            }
            out << ";\n";
        } else {
            out << "return ClxTup" << state.current_multi_count << "{";
            for (uint32_t z = 0; z < state.current_multi_count; ++z)
                out << (z ? ", 0.0" : "0.0");
            out << "};\n";
        }
        return;
    }

    if (state.in_native_impl) {

        if (state.current_arena_func != 0xFFFFFFFF)
            out << "clx::arena_reset(&_arena);\n";
        if (v_count == 0) {
            out << "return clx::LValue();\n";
        } else {
            out << "clx::LValue _rv_" << node_idx << " = ";
            emit_node(ctx.block_statements[first_v]);
            out << ";\n";
            out << "return _rv_" << node_idx << ";\n";
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
        bool ret_is_method = false;
        bool ret_name_short = false;
        if (ctx.nodes[tgt].type == NodeType::TableAccess) {
            if (call_node.as.call_expr.arg_count > 0
                && ctx.block_statements[call_node.as.call_expr.first_arg] == ctx.nodes[tgt].as.table_access.table) {
                ret_is_method = true;
                ret_name_short = true;
            } else {
                ret_name_short = true;
            }
        } else if (ctx.nodes[tgt].type == NodeType::Identifier && ctx.nodes[tgt].as.ident.is_global) {
            ret_name_short = true;
        }
        if (ctx.nodes[tgt].type == NodeType::Identifier && !ctx.nodes[tgt].as.ident.is_global) {
            fname = std::string_view(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (state.direct_callables.count(fname))
                is_direct = true;
        }

        //------------------ unboxed tailcall: triple overload with double-ready args, same shapes as the native tailcall
        if (!fname.empty() && state.in_function_def && !dbl_self_ref(tgt)
            && state.void_funcs.count(fname) == 0
            && dbl_call_eligible(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count)) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
            if (state.in_native_impl) {
                out << "clx::LValue _rv_" << node_idx << " = ";
                try_emit_dbl_call(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count);
                out << ";\n";
                out << "return _rv_" << node_idx << ";\n";
            } else {
                out << "clx::MultiValue _mv_" << node_idx << " = clx::MultiValue(";
                try_emit_dbl_call(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count);
                out << ");\n";
                out << "return _mv_" << node_idx << ";\n";
            }
            return;
        }

        //------------------ native-direct tailcall: exact-1 callee, exact

        //------------------ void callees fall through to generic forwarding: they yield zero values, not one nil
        if (!fname.empty() && state.in_function_def && state.void_funcs.count(fname) == 0
            && native_call_eligible(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count)) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "clx::arena_reset(&_arena);\n";
            if (state.in_native_impl) {
                out << "clx::LValue _rv_" << node_idx << " = ";
                try_emit_native_call(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count, false);
                out << ";\n";
                out << "return _rv_" << node_idx << ";\n";
            } else {
                out << "clx::MultiValue _mv_" << node_idx << " = clx::MultiValue(";
                try_emit_native_call(tgt, call_node.as.call_expr.first_arg, call_node.as.call_expr.arg_count, false);
                out << ");\n";
                out << "return _mv_" << node_idx << ";\n";
            }
            return;
        }

        //------------------ builtin intrinsic alias tail call: raw call before generic dispatch (arity checks stay in the cfunc)
        if (!is_direct && !fname.empty() && state.reassigned_vars.count(std::string(fname)) == 0
            && state.fast_callables.count(std::string(fname)) == 0) {
            auto _ait = state.intrinsic_aliases.find(std::string(fname));
            if (_ait != state.intrinsic_aliases.end() && _ait->second->intrinsic
                && call_node.as.call_expr.arg_count >= _ait->second->min_args) {
                if (state.in_function_def && state.current_arena_func != 0xFFFFFFFF)
                    out << "clx::arena_reset(&_arena);\n";
                ASTNode _tmp { };
                _tmp.type = NodeType::IntrinsicCall;
                _tmp.line = call_node.line;
                _tmp.as.intrinsic_call.cname = _ait->second->intrinsic;
                _tmp.as.intrinsic_call.first_arg = call_node.as.call_expr.first_arg;
                _tmp.as.intrinsic_call.arg_count = call_node.as.call_expr.arg_count;
                if (state.in_function_def) {
                    out << "return clx::MultiValue(";
                    emitIntrinsicCall(_tmp, last_v_idx);
                    out << ");\n";
                } else {
                    out << "return ";
                    emitIntrinsicCall(_tmp, last_v_idx);
                    out << ";\n";
                }
                return;
            }
        }

        out << "{\n";
        //------------------ the call is this function's return value: a yield here leaves nothing but a return to run
        std::string _cs = emit_call_site(call_node.line, ret_is_method, ret_name_short, !func_has_close_guard());
        bool last_expands = false;
        uint32_t last_arg = 0xFFFFFFFF;
        if (call_node.as.call_expr.arg_count > 0) {
            last_arg = ctx.block_statements[call_node.as.call_expr.first_arg + call_node.as.call_expr.arg_count - 1];
            if (ctx.nodes[last_arg].type == NodeType::CallExpression || ctx.nodes[last_arg].type == NodeType::Vararg)
                last_expands = true;
        }

        if (last_expands) {
            out << "    size_t _ssave_dyn = _fr.mark();\n";
            out << "    clx::DynArgs _dyn_buf;\n";
            for (uint32_t a = 0; a < call_node.as.call_expr.arg_count - 1; ++a) {
                if (a > 0)
                    out << "    _fr.push(_dyn_buf[" << (a - 1) << "]);\n";
                out << "    _dyn_buf.push_back(";
                emit_node(ctx.block_statements[call_node.as.call_expr.first_arg + a]);
                out << ");\n";
            }
            state.expect_multivalue = true;
            out << "    clx::MultiValue _mret = ";
            emit_node(last_arg);
            out << ";\n";
            state.expect_multivalue = false;
            out << "    for (size_t _mi = 0; _mi < _mret.count; ++_mi) _dyn_buf.push_back(_mret[_mi]);\n";
            if (is_direct) {
                if (state.in_function_def) {
                    if (state.current_arena_func != 0xFFFFFFFF)
                        out << "    clx::arena_reset(&_arena);\n";
                    out << "    for (size_t i = 0; i < _dyn_buf.size(); ++i) _fr.push(_dyn_buf[i]);\n";
                    out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname)
                        << ", _dyn_buf.data(), _dyn_buf.size());\n";
                    out << "    return _res;\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname)
                        << ", _dyn_buf.data(), _dyn_buf.size());\n";
                    out << "    _fr.restore(_ssave_dyn);\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else if (!fname.empty() && state.builtin_aliases.count(std::string(fname))
                && state.reassigned_vars.count(std::string(fname)) == 0) {
                const std::string &_bi_cf = state.builtin_aliases.at(std::string(fname));
                if (state.in_function_def) {
                    out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf
                        << ", _dyn_buf.data(), _dyn_buf.size());\n";
                    out << "    return _res;\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf
                        << ", _dyn_buf.data(), _dyn_buf.size());\n";
                    out << "    _fr.restore(_ssave_dyn);\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else if (state.in_function_def) {
                if (state.current_arena_func != 0xFFFFFFFF)
                    out << "    clx::arena_reset(&_arena);\n";
                out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                emit_node(tgt);
                out << ", _dyn_buf.data(), _dyn_buf.size(), " << _cs << ");\n";
                out << "    return _res;\n";
            } else {
                out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                emit_node(tgt);
                out << ", _dyn_buf.data(), _dyn_buf.size(), " << _cs << ");\n";
                out << "    _fr.restore(_ssave_dyn);\n";
                out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
            }
        } else {
            if (call_node.as.call_expr.arg_count > 0) {
                out << "    size_t _ssave_ret = _fr.mark();\n";
                out << "    clx::LValue args_" << last_v_idx << "[" << call_node.as.call_expr.arg_count << "];\n";
                for (uint32_t a = 0; a < call_node.as.call_expr.arg_count; ++a) {
                    if (a > 0)
                        out << "    _fr.push(args_" << last_v_idx << "[" << (a - 1) << "]);\n";
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
                        out << "    return _res;\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_cfunc_direct(L, " << impl_call(fname) << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        out << "    _fr.restore(_ssave_ret);\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else if (!fname.empty() && state.builtin_aliases.count(std::string(fname))
                    && state.reassigned_vars.count(std::string(fname)) == 0) {
                    const std::string &_bi_cf = state.builtin_aliases.at(std::string(fname));
                    if (state.in_function_def) {
                        out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        out << "    return _res;\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_cfunc_rooted(L, clx::" << _bi_cf << ", args_"
                            << last_v_idx << ", " << call_node.as.call_expr.arg_count << ");\n";
                        out << "    _fr.restore(_ssave_ret);\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else if (state.in_function_def) {
                    if (state.current_arena_func != 0xFFFFFFFF)
                        out << "    clx::arena_reset(&_arena);\n";
                    out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                    emit_node(tgt);
                    out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", " << _cs << ");\n";
                    out << "    return _res;\n";
                } else {
                    out << "    clx::MultiValue _res = clx::call_function_rooted(L, ";
                    emit_node(tgt);
                    out << ", args_" << last_v_idx << ", " << call_node.as.call_expr.arg_count << ", " << _cs << ");\n";
                    out << "    _fr.restore(_ssave_ret);\n";
                    out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                }
            } else {
                if (is_direct) {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    CLX_MUSTTAIL return " << impl_call(fname) << "(L, nullptr, 0);\n";
                    } else {
                        out << "    clx::MultiValue _res = " << impl_call(fname) << "(L, nullptr, 0);\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else if (!fname.empty() && state.builtin_aliases.count(std::string(fname))
                    && state.reassigned_vars.count(std::string(fname)) == 0) {
                    const std::string &_bi_cf = state.builtin_aliases.at(std::string(fname));
                    if (state.in_function_def) {
                        out << "    return clx::" << _bi_cf << "(L, nullptr, 0);\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::" << _bi_cf << "(L, nullptr, 0);\n";
                        out << "    return (_res.count > 0) ? _res[0] : clx::LValue();\n";
                    }
                } else {
                    if (state.in_function_def) {
                        if (state.current_arena_func != 0xFFFFFFFF)
                            out << "    clx::arena_reset(&_arena);\n";
                        out << "    clx::MultiValue _res = clx::call_function(L, ";
                        emit_node(tgt);
                        out << ", nullptr, 0, " << _cs << ");\n";
                        out << "    return _res;\n";
                    } else {
                        out << "    clx::MultiValue _res = clx::call_function(L, ";
                        emit_node(tgt);
                        out << ", nullptr, 0, " << _cs << ");\n";
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
        out << "    size_t _rg_rbase = _fr.mark();\n";
        out << "    clx::LValue _ret_args[" << std::max(1u, v_count) << "];\n";
        for (size_t i = 0; i < v_count; ++i) {
            out << "    _ret_args[" << i << "] = ";
            emit_node(ctx.block_statements[first_v + i]);
            out << ";\n";
            out << "    _fr.push(_ret_args[" << i << "]);\n";
        }
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "    clx::arena_reset(&_arena);\n";
            out << "    return clx::MultiValue(_ret_args, " << v_count << ", L);\n";
        } else {
            out << "    _fr.restore(_rg_rbase);\n";
            out << "    return _ret_args[0];\n";
        }
    } else {
        out << "    size_t _rg_rbase = _fr.mark();\n";
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
                out << "        _fr.push(_mret[m]);\n";
                out << "    }\n";
            } else {
                out << "    clx::LValue _tmp_" << node_idx << "_" << i << " = ";
                emit_node(v_idx);
                out << ";\n";
                out << "    _ret_vals.push_back(_tmp_" << node_idx << "_" << i << ");\n";
                out << "    _fr.push(_tmp_" << node_idx << "_" << i << ");\n";
            }
        }
        if (state.in_function_def) {
            if (state.current_arena_func != 0xFFFFFFFF)
                out << "    clx::arena_reset(&_arena);\n";
            out << "    return clx::MultiValue(_ret_vals, L);\n";
        } else {
            out << "    _fr.restore(_rg_rbase);\n";
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
    CmpSwapMatch _csm;
    if (try_match_cmp_swap(node_idx, _csm)) {
        emit_cmp_swap(_csm, node_idx);
        if (node.as.if_stmt.else_block != 0xFFFFFFFF) {
            out << "else\n";
            emit_node(node.as.if_stmt.else_block);
        }
        return;
    }
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
