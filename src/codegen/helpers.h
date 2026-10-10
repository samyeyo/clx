// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  helpers.h · Shared codegen helpers         │
// └─────────────────────────────────────────────┘

#pragma once

#include "codegen.h"
#include <functional>
#include <set>
#include <string>
#include <string_view>

namespace clx {

//------------------ is_zero_number_literal: is a node a numeric literal equal to zero?
bool is_zero_number_literal(const ASTContext &ctx, uint32_t n_idx);

//------------------ collect_string_builder_refs: walk an argument subtree and collect string-builder locals
void collect_string_builder_refs(
    const ASTContext &ctx, uint32_t n_idx, const std::set<std::string_view> &sb_set, std::set<std::string_view> &out);

//------------------ collect_hoisted_tables: table-typed locals indexed under a loop body (nested functions pruned, plain identifiers only).
void collect_hoisted_tables(const ASTContext &ctx, uint32_t n_idx, const std::set<std::string, std::less<>> &tbl_set,
    std::set<std::string_view> &out);

//------------------ OwnScan: result of scanning a statement for a local used in a by-reference position
struct OwnScan {
    bool materialized = false;
};

//------------------ scan_own: `name` where emitted text binds a by-ref operand (in memory anyway; rooting wasted); nested blocks via on_block.
void scan_own(const ASTContext &ctx, const AnalysisState &state, uint32_t idx, std::string_view name, OwnScan &res,
    std::function<void(uint32_t)> &on_block, bool by_ref);

//------------------ local_register_friendly: no use passes the local by reference, so snapshot re-rooting keeps it in a register.
bool local_register_friendly(
    const ASTContext &ctx, const AnalysisState &state, uint32_t body_idx, std::string_view name);

//------------------ kIdentUseUnsafe: sentinel returned by count_ident_uses when a subtree cannot be analysed
constexpr int kIdentUseUnsafe = 1000;

//------------------ count_ident_uses: occurrences of `name` as an Identifier inside a statement subtree, excluding two
//------------------ known node indices. Returns kIdentUseUnsafe when any unhandled node type is met, so callers can
//------------------ treat "unknown" as "unsafe" and keep the conservative path.
int count_ident_uses(const ASTContext &ctx, uint32_t idx, std::string_view name, uint32_t skip_a, uint32_t skip_b);

//------------------ lua_decode_string: decodes Lua escape sequences in a string literal body
std::string lua_decode_string(std::string_view s);

//------------------ cpp_escape: escapes a string for embedding in a C++ string literal
std::string cpp_escape(std::string_view s);

}
