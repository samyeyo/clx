// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  helpers.cpp · Shared codegen free helpers  │
// └─────────────────────────────────────────────┘

#ifdef _WIN32
#define NOMINMAX
#endif
#include "codegen.h"
#include "helpers.h"
#include "../../include/clx.h"
#include "../optimizer/optimizer.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clx {

//------------------ is_zero_number_literal: is a node a numeric literal equal to zero?

bool is_zero_number_literal(const ASTContext &ctx, uint32_t n_idx) {
    const ASTNode &n = ctx.nodes[n_idx];
    return (n.type == NodeType::Number && n.as.number.val == 0.0)
        || (n.type == NodeType::Integer && n.as.integer.val == 0);
}

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

//------------------ collect_hoisted_tables: table-typed locals indexed under a loop body (nested functions pruned, plain identifiers only).
void collect_hoisted_tables(const ASTContext &ctx, uint32_t n_idx, const std::set<std::string, std::less<>> &tbl_set,
    std::set<std::string_view> &out) {
    if (n_idx == 0xFFFFFFFF || n_idx >= ctx.nodes.size())
        return;
    const auto &nd = ctx.nodes[n_idx];
    if (nd.type == NodeType::FunctionDef)
        return;
    if (nd.type == NodeType::TableAccess) {
        uint32_t t = nd.as.table_access.table;
        if (t < ctx.nodes.size() && ctx.nodes[t].type == NodeType::Identifier && !ctx.nodes[t].as.ident.is_global) {
            std::string_view nm(ctx.nodes[t].as.ident.name, ctx.nodes[t].as.ident.length);
            std::string key(nm);
            if (tbl_set.count(key))
                out.insert(nm);
        }
        collect_hoisted_tables(ctx, nd.as.table_access.table, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.table_access.key, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::Block) {
        for (uint32_t i = 0; i < nd.as.block.count; ++i)
            collect_hoisted_tables(ctx, ctx.block_statements[nd.as.block.first_statement + i], tbl_set, out);
        return;
    }
    if (nd.type == NodeType::LocalDecl || nd.type == NodeType::GlobalDeclStatement || nd.type == NodeType::Assignment) {
        uint32_t vc = (nd.type == NodeType::LocalDecl)
            ? nd.as.local_decl.value_count
            : ((nd.type == NodeType::GlobalDeclStatement) ? nd.as.global_decl.value_count : nd.as.assign.value_count);
        uint32_t fv = (nd.type == NodeType::LocalDecl)
            ? nd.as.local_decl.first_value
            : ((nd.type == NodeType::GlobalDeclStatement) ? nd.as.global_decl.first_value : nd.as.assign.first_value);
        for (uint32_t i = 0; i < vc; ++i)
            collect_hoisted_tables(ctx, ctx.block_statements[fv + i], tbl_set, out);
        if (nd.type == NodeType::Assignment) {
            for (uint32_t i = 0; i < nd.as.assign.target_count; ++i)
                collect_hoisted_tables(ctx, ctx.block_statements[nd.as.assign.first_target + i], tbl_set, out);
        }
        return;
    }
    if (nd.type == NodeType::BinaryOp) {
        collect_hoisted_tables(ctx, nd.as.bin_op.left, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.bin_op.right, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::UnaryOp) {
        collect_hoisted_tables(ctx, nd.as.unary_op.expr, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::ParenExpression) {
        collect_hoisted_tables(ctx, nd.as.paren_expr.expr, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::CallExpression) {
        collect_hoisted_tables(ctx, nd.as.call_expr.target, tbl_set, out);
        for (uint32_t i = 0; i < nd.as.call_expr.arg_count; ++i)
            collect_hoisted_tables(ctx, ctx.block_statements[nd.as.call_expr.first_arg + i], tbl_set, out);
        return;
    }
    if (nd.type == NodeType::IfStatement) {
        collect_hoisted_tables(ctx, nd.as.if_stmt.condition, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.if_stmt.then_block, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.if_stmt.else_block, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::WhileStatement) {
        collect_hoisted_tables(ctx, nd.as.while_stmt.condition, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.while_stmt.body_block, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::RepeatStatement) {
        collect_hoisted_tables(ctx, nd.as.repeat_stmt.body_block, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.repeat_stmt.condition, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::ForStatement) {
        collect_hoisted_tables(ctx, nd.as.for_stmt.start_expr, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.for_stmt.limit_expr, tbl_set, out);
        if (nd.as.for_stmt.step_expr != 0xFFFFFFFF)
            collect_hoisted_tables(ctx, nd.as.for_stmt.step_expr, tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.for_stmt.body_block, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::GenericForStatement) {
        for (uint32_t i = 0; i < nd.as.generic_for.iter_count; ++i)
            collect_hoisted_tables(ctx, ctx.block_statements[nd.as.generic_for.first_iter + i], tbl_set, out);
        collect_hoisted_tables(ctx, nd.as.generic_for.body_block, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::DoStatement) {
        collect_hoisted_tables(ctx, nd.as.do_stmt.body_block, tbl_set, out);
        return;
    }
    if (nd.type == NodeType::TableConstructor) {
        for (uint32_t i = 0; i < nd.as.table_cons.count; ++i) {
            collect_hoisted_tables(ctx, ctx.block_statements[nd.as.table_cons.first_item + i * 2], tbl_set, out);
            collect_hoisted_tables(ctx, ctx.block_statements[nd.as.table_cons.first_item + i * 2 + 1], tbl_set, out);
        }
        return;
    }
    if (nd.type == NodeType::ReturnStatement) {
        for (uint32_t i = 0; i < nd.as.return_stmt.value_count; ++i)
            collect_hoisted_tables(ctx, ctx.block_statements[nd.as.return_stmt.first_value + i], tbl_set, out);
        return;
    }
}

//------------------ scan_own: `name` where emitted text binds a by-ref operand (in memory anyway; rooting wasted); nested blocks via on_block.
void scan_own(const ASTContext &ctx, const AnalysisState &state, uint32_t idx, std::string_view name, OwnScan &res,
    std::function<void(uint32_t)> &on_block, bool by_ref) {
    if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
        return;
    const auto &nd = ctx.nodes[idx];
    switch (nd.type) {
    case NodeType::FunctionDef:
    case NodeType::Number:
    case NodeType::Integer:
    case NodeType::String:
    case NodeType::TrueLiteral:
    case NodeType::FalseLiteral:
    case NodeType::NilLiteral:
    case NodeType::LabelStatement:
    case NodeType::GotoStatement:
    case NodeType::BreakStatement:
    case NodeType::Vararg:
        return;
    case NodeType::Block:
        on_block(idx);
        return;
    case NodeType::Identifier: {
        std::string_view nm(nd.as.ident.name, nd.as.ident.length);
        if (by_ref && !nd.as.ident.is_global && nm == name)
            res.materialized = true;
        return;
    }
    case NodeType::CallExpression:
        scan_own(ctx, state, nd.as.call_expr.target, name, res, on_block, true);
        for (uint32_t i = 0; i < nd.as.call_expr.arg_count; ++i)
            scan_own(ctx, state, ctx.block_statements[nd.as.call_expr.first_arg + i], name, res, on_block, false);
        return;
    case NodeType::IntrinsicCall:
        for (uint32_t i = 0; i < nd.as.intrinsic_call.arg_count; ++i)
            scan_own(ctx, state, ctx.block_statements[nd.as.intrinsic_call.first_arg + i], name, res, on_block, false);
        return;
    case NodeType::GenericForStatement:
        for (uint32_t i = 0; i < nd.as.generic_for.iter_count; ++i)
            scan_own(ctx, state, ctx.block_statements[nd.as.generic_for.first_iter + i], name, res, on_block, true);
        scan_own(ctx, state, nd.as.generic_for.body_block, name, res, on_block, false);
        return;
    case NodeType::TableAccess: {
        uint32_t tbl = nd.as.table_access.table;
        uint32_t key = nd.as.table_access.key;
        bool table_ok = tbl < ctx.nodes.size() && ctx.nodes[tbl].type == NodeType::Identifier
            && !ctx.nodes[tbl].as.ident.is_global;
        bool key_ok
            = state.bce_safe_nodes.count(idx) > 0 || yields_number(ctx, state, key, nullptr, state.current_fast_func);
        bool operands_by_ref = !table_ok || !key_ok;
        scan_own(ctx, state, tbl, name, res, on_block, operands_by_ref);
        scan_own(ctx, state, key, name, res, on_block, operands_by_ref);
        return;
    }
    case NodeType::BinaryOp:
    case NodeType::UnaryOp: {
        uint32_t left = (nd.type == NodeType::BinaryOp) ? nd.as.bin_op.left : nd.as.unary_op.expr;
        uint32_t right = (nd.type == NodeType::BinaryOp) ? nd.as.bin_op.right : 0xFFFFFFFF;
        bool native = yields_number(ctx, state, left, nullptr, state.current_fast_func)
            && (right == 0xFFFFFFFF || yields_number(ctx, state, right, nullptr, state.current_fast_func));
        scan_own(ctx, state, left, name, res, on_block, !native);
        scan_own(ctx, state, right, name, res, on_block, !native);
        return;
    }
    case NodeType::ParenExpression:
        scan_own(ctx, state, nd.as.paren_expr.expr, name, res, on_block, by_ref);
        return;
    case NodeType::LocalDecl:
    case NodeType::GlobalDeclStatement: {
        uint32_t vc = (nd.type == NodeType::LocalDecl) ? nd.as.local_decl.value_count : nd.as.global_decl.value_count;
        uint32_t fv = (nd.type == NodeType::LocalDecl) ? nd.as.local_decl.first_value : nd.as.global_decl.first_value;
        for (uint32_t i = 0; i < vc; ++i)
            scan_own(ctx, state, ctx.block_statements[fv + i], name, res, on_block, false);
        return;
    }
    case NodeType::Assignment: {
        for (uint32_t i = 0; i < nd.as.assign.target_count; ++i) {
            uint32_t t = ctx.block_statements[nd.as.assign.first_target + i];
            uint32_t v
                = (i < nd.as.assign.value_count) ? ctx.block_statements[nd.as.assign.first_value + i] : 0xFFFFFFFF;
            bool value_by_ref = false;
            if (t < ctx.nodes.size() && ctx.nodes[t].type == NodeType::TableAccess) {
                uint32_t tbl = ctx.nodes[t].as.table_access.table;
                bool plain_tbl = tbl < ctx.nodes.size() && ctx.nodes[tbl].type == NodeType::Identifier
                    && !ctx.nodes[tbl].as.ident.is_global;
                value_by_ref = plain_tbl && state.bce_safe_nodes.count(t) == 0;
            }
            scan_own(ctx, state, t, name, res, on_block, false);
            scan_own(ctx, state, v, name, res, on_block, value_by_ref);
        }
        return;
    }
    case NodeType::IfStatement:
        scan_own(ctx, state, nd.as.if_stmt.condition, name, res, on_block, false);
        scan_own(ctx, state, nd.as.if_stmt.then_block, name, res, on_block, false);
        scan_own(ctx, state, nd.as.if_stmt.else_block, name, res, on_block, false);
        return;
    case NodeType::WhileStatement:
        scan_own(ctx, state, nd.as.while_stmt.condition, name, res, on_block, false);
        scan_own(ctx, state, nd.as.while_stmt.body_block, name, res, on_block, false);
        return;
    case NodeType::RepeatStatement:
        scan_own(ctx, state, nd.as.repeat_stmt.body_block, name, res, on_block, false);
        scan_own(ctx, state, nd.as.repeat_stmt.condition, name, res, on_block, false);
        return;
    case NodeType::ForStatement:
        scan_own(ctx, state, nd.as.for_stmt.start_expr, name, res, on_block, false);
        scan_own(ctx, state, nd.as.for_stmt.limit_expr, name, res, on_block, false);
        if (nd.as.for_stmt.step_expr != 0xFFFFFFFF)
            scan_own(ctx, state, nd.as.for_stmt.step_expr, name, res, on_block, false);
        scan_own(ctx, state, nd.as.for_stmt.body_block, name, res, on_block, false);
        return;
    case NodeType::DoStatement:
        scan_own(ctx, state, nd.as.do_stmt.body_block, name, res, on_block, false);
        return;
    case NodeType::TableConstructor:
        for (uint32_t i = 0; i < nd.as.table_cons.count; ++i) {
            scan_own(ctx, state, ctx.block_statements[nd.as.table_cons.first_item + i * 2], name, res, on_block, false);
            scan_own(
                ctx, state, ctx.block_statements[nd.as.table_cons.first_item + i * 2 + 1], name, res, on_block, true);
        }
        return;
    case NodeType::ReturnStatement:
        for (uint32_t i = 0; i < nd.as.return_stmt.value_count; ++i)
            scan_own(ctx, state, ctx.block_statements[nd.as.return_stmt.first_value + i], name, res, on_block, false);
        return;
    default:
        res.materialized = true;
        return;
    }
}

//------------------ local_register_friendly: no use passes the local by reference, so snapshot re-rooting keeps it in a register.
bool local_register_friendly(
    const ASTContext &ctx, const AnalysisState &state, uint32_t body_idx, std::string_view name) {
    if (body_idx == 0xFFFFFFFF || body_idx >= ctx.nodes.size())
        return false;
    const auto &blk = ctx.nodes[body_idx];
    if (blk.type != NodeType::Block)
        return false;
    OwnScan res;
    std::function<void(uint32_t)> on_block = [&](uint32_t b) {
        if (!local_register_friendly(ctx, state, b, name))
            res.materialized = true;
    };
    for (uint32_t i = 0; i < blk.as.block.count && !res.materialized; ++i) {
        uint32_t stmt = ctx.block_statements[blk.as.block.first_statement + i];
        scan_own(ctx, state, stmt, name, res, on_block, false);
    }
    return !res.materialized;
}

//------------------ count_ident_uses: occurrences of `name` as an Identifier inside a statement subtree, excluding two
//------------------ known node indices. Returns kIdentUseUnsafe when any unhandled node type is met, so callers can
//------------------ treat "unknown" as "unsafe" and keep the conservative path.

int count_ident_uses(const ASTContext &ctx, uint32_t idx, std::string_view name, uint32_t skip_a, uint32_t skip_b) {
    if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
        return 0;
    const auto &nd = ctx.nodes[idx];
    auto sum = [&](int a, int b) { return (a >= kIdentUseUnsafe || b >= kIdentUseUnsafe) ? kIdentUseUnsafe : a + b; };
    switch (nd.type) {
    case NodeType::Identifier: {
        std::string_view nm(nd.as.ident.name, nd.as.ident.length);
        if (nm != name || idx == skip_a || idx == skip_b)
            return 0;
        //------------------ a captured name would be an upvalue, which this scan cannot account for
        return nd.as.ident.is_captured ? kIdentUseUnsafe : 1;
    }
    case NodeType::Number:
    case NodeType::Integer:
    case NodeType::String:
    case NodeType::TrueLiteral:
    case NodeType::FalseLiteral:
    case NodeType::NilLiteral:
    case NodeType::LabelStatement:
    case NodeType::GotoStatement:
    case NodeType::BreakStatement:
    case NodeType::Vararg:
        return 0;
    case NodeType::FunctionDef:
        return kIdentUseUnsafe;
    case NodeType::Block: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.block.count; ++i) {
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.block.first_statement + i], name, skip_a, skip_b));
            if (total >= kIdentUseUnsafe)
                return kIdentUseUnsafe;
        }
        return total;
    }
    case NodeType::CallExpression: {
        int total = count_ident_uses(ctx, nd.as.call_expr.target, name, skip_a, skip_b);
        for (uint32_t i = 0; i < nd.as.call_expr.arg_count; ++i)
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.call_expr.first_arg + i], name, skip_a, skip_b));
        return total;
    }
    case NodeType::IntrinsicCall: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.intrinsic_call.arg_count; ++i)
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.intrinsic_call.first_arg + i], name, skip_a, skip_b));
        return total;
    }
    case NodeType::GenericForStatement: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.generic_for.iter_count; ++i)
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.generic_for.first_iter + i], name, skip_a, skip_b));
        return sum(total, count_ident_uses(ctx, nd.as.generic_for.body_block, name, skip_a, skip_b));
    }
    case NodeType::TableAccess:
        return sum(count_ident_uses(ctx, nd.as.table_access.table, name, skip_a, skip_b),
            count_ident_uses(ctx, nd.as.table_access.key, name, skip_a, skip_b));
    case NodeType::BinaryOp:
        return sum(count_ident_uses(ctx, nd.as.bin_op.left, name, skip_a, skip_b),
            count_ident_uses(ctx, nd.as.bin_op.right, name, skip_a, skip_b));
    case NodeType::UnaryOp:
        return count_ident_uses(ctx, nd.as.unary_op.expr, name, skip_a, skip_b);
    case NodeType::ParenExpression:
        return count_ident_uses(ctx, nd.as.paren_expr.expr, name, skip_a, skip_b);
    case NodeType::LocalDecl: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.local_decl.value_count; ++i)
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.local_decl.first_value + i], name, skip_a, skip_b));
        return total;
    }
    case NodeType::GlobalDeclStatement: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.global_decl.value_count; ++i)
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.global_decl.first_value + i], name, skip_a, skip_b));
        return total;
    }
    case NodeType::Assignment: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.assign.target_count; ++i) {
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.assign.first_target + i], name, skip_a, skip_b));
            if (i < nd.as.assign.value_count)
                total = sum(total,
                    count_ident_uses(ctx, ctx.block_statements[nd.as.assign.first_value + i], name, skip_a, skip_b));
        }
        return total;
    }
    case NodeType::IfStatement:
        return sum(sum(count_ident_uses(ctx, nd.as.if_stmt.condition, name, skip_a, skip_b),
                       count_ident_uses(ctx, nd.as.if_stmt.then_block, name, skip_a, skip_b)),
            count_ident_uses(ctx, nd.as.if_stmt.else_block, name, skip_a, skip_b));
    case NodeType::WhileStatement:
        return sum(count_ident_uses(ctx, nd.as.while_stmt.condition, name, skip_a, skip_b),
            count_ident_uses(ctx, nd.as.while_stmt.body_block, name, skip_a, skip_b));
    case NodeType::RepeatStatement:
        return sum(count_ident_uses(ctx, nd.as.repeat_stmt.body_block, name, skip_a, skip_b),
            count_ident_uses(ctx, nd.as.repeat_stmt.condition, name, skip_a, skip_b));
    case NodeType::ForStatement: {
        int total = sum(count_ident_uses(ctx, nd.as.for_stmt.start_expr, name, skip_a, skip_b),
            count_ident_uses(ctx, nd.as.for_stmt.limit_expr, name, skip_a, skip_b));
        if (nd.as.for_stmt.step_expr != 0xFFFFFFFF)
            total = sum(total, count_ident_uses(ctx, nd.as.for_stmt.step_expr, name, skip_a, skip_b));
        return sum(total, count_ident_uses(ctx, nd.as.for_stmt.body_block, name, skip_a, skip_b));
    }
    case NodeType::DoStatement:
        return count_ident_uses(ctx, nd.as.do_stmt.body_block, name, skip_a, skip_b);
    case NodeType::TableConstructor: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.table_cons.count; ++i) {
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.table_cons.first_item + i * 2], name, skip_a, skip_b));
            total = sum(total,
                count_ident_uses(
                    ctx, ctx.block_statements[nd.as.table_cons.first_item + i * 2 + 1], name, skip_a, skip_b));
        }
        return total;
    }
    case NodeType::ReturnStatement: {
        int total = 0;
        for (uint32_t i = 0; i < nd.as.return_stmt.value_count; ++i)
            total = sum(total,
                count_ident_uses(ctx, ctx.block_statements[nd.as.return_stmt.first_value + i], name, skip_a, skip_b));
        return total;
    }
    default:
        return kIdentUseUnsafe;
    }
}

//------------------ hex_digit_value: numeric value of a single hexadecimal digit character
static int hex_digit_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    return (c | 0x20) - 'a' + 10;
}

std::string lua_decode_string(std::string_view s) {
    std::string r;
    r.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\n' || s[i] == '\r')
            throw std::runtime_error("unfinished string");
        if (s[i] == '\\') {
            if (i + 1 >= s.length())
                throw std::runtime_error("unfinished string");
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
            case '\n':
            case '\r': {
                unsigned char first = c;
                r += '\n';
                i++;
                if (i + 1 < s.length() && (s[i + 1] == '\n' || s[i + 1] == '\r')
                    && static_cast<unsigned char>(s[i + 1]) != first)
                    i++;
                break;
            }
            case 'z': {
                i += 2;
                while (i < s.length() && std::isspace(static_cast<unsigned char>(s[i])))
                    i++;
                i--;
                break;
            }
            case 'x': {
                if (i + 3 >= s.length() || !std::isxdigit(static_cast<unsigned char>(s[i + 2]))
                    || !std::isxdigit(static_cast<unsigned char>(s[i + 3])))
                    throw std::runtime_error("hexadecimal digit expected");
                r += static_cast<char>((hex_digit_value(static_cast<unsigned char>(s[i + 2])) << 4)
                    | hex_digit_value(static_cast<unsigned char>(s[i + 3])));
                i += 3;
                break;
            }
            case 'u': {
                if (i + 2 >= s.length() || s[i + 2] != '{')
                    throw std::runtime_error("missing '{'");
                size_t j = i + 3;
                if (j >= s.length() || !std::isxdigit(static_cast<unsigned char>(s[j])))
                    throw std::runtime_error("hexadecimal digit expected");
                unsigned long cp = 0;
                while (j < s.length() && std::isxdigit(static_cast<unsigned char>(s[j]))) {
                    if (cp > 0x07FFFFFFul)
                        throw std::runtime_error("UTF-8 value too large");
                    cp = (cp << 4) | static_cast<unsigned long>(hex_digit_value(static_cast<unsigned char>(s[j])));
                    j++;
                }
                if (j >= s.length() || s[j] != '}')
                    throw std::runtime_error("missing '}'");
                if (cp < 0x80) {
                    r += static_cast<char>(cp);
                } else {
                    unsigned char cont[8];
                    int k = 0;
                    unsigned long mfb = 0x3f;
                    do {
                        cont[k++] = static_cast<unsigned char>(0x80 | (cp & 0x3f));
                        cp >>= 6;
                        mfb >>= 1;
                    } while (cp > mfb);
                    r += static_cast<char>((~mfb << 1) | cp);
                    while (k > 0)
                        r += static_cast<char>(cont[--k]);
                }
                i = j;
                break;
            }
            default: {
                if (c >= '0' && c <= '9') {
                    size_t j = i + 1;
                    while (j < s.length() && j - i <= 3 && s[j] >= '0' && s[j] <= '9')
                        j++;
                    std::string dec(s.data() + i + 1, j - i - 1);
                    long val = std::strtol(dec.c_str(), nullptr, 10);
                    if (val > 255)
                        throw std::runtime_error("decimal escape too large");
                    r += static_cast<char>(val);
                    i = j - 1;
                } else {
                    throw std::runtime_error("invalid escape sequence");
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

std::string cpp_escape(std::string_view s) {
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
            if (c < 0x20 || c >= 0x80) {
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

}
