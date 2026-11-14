// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  analysis_state.h · Shared Analysis State   │
// └─────────────────────────────────────────────┘

#ifndef ANALYSIS_STATE_H
#define ANALYSIS_STATE_H

#include "../builtins.h"
#include "../syntax/nodes.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace clx {

//------------------ FieldKind: statically known kind of a promoted struct field value
enum class FieldKind : uint8_t { Unknown = 0, Int, Double, Str };

//------------------ StructLocal: record local compiled to a native C++ struct — ordered fields with kinds
struct StructLocal {
    std::vector<std::string> fields;
    std::vector<FieldKind> kinds;
    std::vector<bool> late;
    uint32_t decl_node = 0xFFFFFFFF;
};

//------------------ StructShape: one emitted C++ struct type — ordered members shared by same-shaped locals
struct StructShape {
    std::string name;
    std::vector<std::string> fields;
    std::vector<FieldKind> kinds;
    std::vector<bool> late;
};

struct AnalysisState {
    //------------------ Whole-program facts discovered by the optimizer
    std::vector<std::string_view> native_numbers;
    std::vector<std::string_view> string_pool;
    std::unordered_map<std::string_view, size_t> string_pool_index;
    std::set<std::string_view> native_return_funcs;
    std::map<uint32_t, std::set<std::string>> param_numbers;
    std::vector<std::string> param_names;
    std::map<uint32_t, uint32_t> table_presize;
    std::map<std::string_view, double> global_constants;
    std::set<uint32_t> bce_safe_nodes;
    std::set<std::string_view> pure_numeric_arrays;
    std::set<std::string_view> tables_with_dynamic_length;
    std::map<std::string_view, size_t> known_table_lengths;
    std::map<std::string_view, std::set<uint32_t>> pure_numeric_func_params;
    std::map<uint32_t, uint32_t> node_func_owner;
    std::set<std::string, std::less<>> native_integers;
    std::set<std::string_view> int_returning_funcs;
    std::map<std::string_view, uint32_t> int_preserving_masks;
    std::set<std::string, std::less<>> int_typed_locals;
    std::set<std::string, std::less<>> table_typed_locals;

    //------------------ Arena analysis data
    std::set<std::string_view> escaping_vars;
    std::set<uint32_t> arena_safe_table_nodes;
    std::map<uint32_t, uint32_t> arena_table_sizes;

    //------------------ Per-function analysis data
    std::map<std::pair<uint32_t, std::string_view>, std::set<std::string_view>> numeric_table_fields;
    //------------------ struct promotion: locals compiled to native structs instead of LTable
    //------------------ (key owns its name: candidate vectors are destroyed after the pass)
    std::map<std::pair<uint32_t, std::string>, StructLocal> struct_locals;
    //------------------ goto_crossed_locals: locals whose initializer can be skipped by a goto (from pass_local_hygiene)
    std::set<std::string_view> goto_crossed_locals;
    //------------------ struct_proven_reads: late-field read nodes with a dominating earlier write
    std::set<uint32_t> struct_proven_reads;
    //------------------ struct_shapes: per distinct shape, type name + members for declaration emission
    std::map<std::string, StructShape> struct_shapes;
    //------------------ zero_index_tables: fresh {__index=0}-metatable locals; fused t[k]=t[k]<op>const skips __index on miss.
    std::set<std::pair<uint32_t, std::string_view>> zero_index_tables;
    //------------------ dead_stmts: single-occurrence literal-only table decls, never observable; codegen drops them.
    std::set<uint32_t> dead_stmts;
    std::set<std::string_view> direct_callables;
    std::set<std::string_view> fast_callables;
    std::set<std::string_view> native_direct_funcs;
    //------------------ void_funcs: native-direct callees with no ReturnStatement at all. Their impl
    //------------------ falls through to the trailing nil return (stock: zero values). Single-value call
    //------------------ sites use the impl directly; multi-value sites discard it via the comma operator.
    std::set<std::string_view> void_funcs;
    std::map<std::string, uint32_t, std::less<>> multi_native_return_counts;
    std::set<std::string, std::less<>> numeric_builtin_aliases;
    std::map<std::string_view, uint32_t> func_param_counts;
    std::map<std::string_view, std::vector<bool>> func_param_native;
    //------------------ func_param_body: snapshot of func_param_native before call-site argument
    //------------------ proofs disqualify params; identifies functions whose bodies can use a fast variant.
    std::map<std::string_view, std::vector<bool>> func_param_body;
    //------------------ fast_body_funcs: functions eligible for a native-double fast variant.
    std::set<std::string_view> fast_body_funcs;
    //------------------ fast_call_sites: CallExpression nodes whose args are proven numeric, so a
    //------------------ mixed function (fast variant + generic closure) can take the fast path here.
    std::set<uint32_t> fast_call_sites;
    //------------------ slow_call_funcs: fast-body functions with at least one unproven call site;
    //------------------ they emit both the fast variant and a fully generic closure body.
    std::set<std::string_view> slow_call_funcs;
    //------------------ generic_body_func: FunctionDef whose body is currently emitted as the generic
    //------------------ closure of a mixed function; params stay LValue and fast calls are suppressed.
    uint32_t generic_body_func = 0xFFFFFFFF;
    std::set<std::string_view> reassigned_vars;
    std::set<std::string_view> assigned_targets;
    //------------------ nogc_locals: (owner scope, local name) proven to never hold a GC-managed
    //------------------ value (only numbers, booleans, nil, strings — none traced by the collector).
    //------------------ Codegen skips their frame slot entirely. Owner is the FunctionDef node
    //------------------ index, or 0xFFFFFFFF for file scope. Filled by pass_nogc_locals.
    std::set<std::pair<uint32_t, std::string_view>> nogc_locals;
    std::set<std::string_view> int_numeric_arrays;
    //------------------ for_counter_names: numeric-for loop counter identifiers. Codegen emits
    std::set<std::string_view> for_counter_names;
    //------------------ referenced_globals: global names read by generated code, filled by codegen
    //------------------ identifier emission. The driver unions them across modules to emit filtered
    //------------------ std registration (only referenced base functions get linked). Owning strings:
    //------------------ the per-file AST backing string_views dies before main is emitted.
    std::set<std::string> referenced_globals;
    //------------------ uses_dynamic_env: user code reads _G/_ENV as a value (dynamic global access
    //------------------ invisible to referenced_globals). Forces full std registration.
    bool uses_dynamic_env = false;
    std::set<std::string_view> constant_upvalues;
    std::set<std::string_view> string_builders;
    std::set<std::string_view> global_string_builders;
    std::set<std::string_view> module_string_builders;
    std::map<uint32_t, uint32_t> goto_targets;
    std::set<std::string_view> hoisted_locals;
    std::unordered_map<uint32_t, std::string> hoisted_lookups;
    std::unordered_map<std::string, std::string> hoisted_cfuncs;
    std::unordered_map<std::string, std::string> builtin_aliases;
    //------------------ intrinsic_aliases: local bound to an intrinsically lowered std function
    //------------------ (alias name -> registry entry); call sites lower to raw C++ calls.
    std::map<std::string, const BuiltinEntry *, std::less<>> intrinsic_aliases;
    //------------------ builtin_field_writes: "module.field" keys written somewhere in this
    //------------------ module's own code, disqualifying static lowering for that key.
    std::set<std::string, std::less<>> builtin_field_writes;
    //------------------ builtin_module_writes: std module globals reassigned wholesale ("math = {}"
    //------------------ or a dynamic-key write), disqualifying every lowering that reads that module.
    std::set<std::string, std::less<>> builtin_module_writes;
    std::vector<std::string> const_snapshot_cells;

    //------------------ Codegen emission state (mutated as CodeEmitter walks the tree)
    bool skip_block_braces = false;
    bool emit_raw_lambda = false;
    bool emit_fast_lambda = false;
    bool emit_native_impl = false;
    bool in_function_def = false;
    bool in_fast_function = false;
    //------------------ current_vararg_table: cpp name of the enclosing function's named vararg table
    //------------------ ("function f(...t)"); `...` then expands that table instead of the raw arguments
    std::string current_vararg_table;
    bool expect_multivalue = false;
    bool in_native_impl = false;
    //------------------ dbl-overload emission state: emit_dbl_impl selects the triple-bound
    //------------------ prologue while emitting a FunctionDef body; in_dbl_impl marks calls/returns
    //------------------ inside a triple-bound body (returns take the native path, self-calls stay boxed).
    bool emit_dbl_impl = false;
    bool in_dbl_impl = false;
    bool emit_multi_impl = false;
    bool in_multi_impl = false;
    uint32_t current_multi_count = 0;
    std::string_view current_multi_func;
    std::string_view current_native_func;
    std::set<std::string_view> native_emitted;
    //------------------ native_dbl_emitted: native-direct callees with a triple-bound (double,
    //------------------ int-flag, int-shadow) overload emitted; scoped exactly like native_emitted.
    std::set<std::string_view> native_dbl_emitted;
    std::set<std::string_view> multi_emitted;
    std::unordered_map<std::string_view, std::string> hoisted_tables;
    std::string_view current_fast_func;
    std::string ref_capture;
    std::string raw_lambda_cell;
    std::set<std::string_view> holder_cell_callables;
    std::map<std::pair<uint32_t, std::string_view>, bool> register_friendly_locals;
    uint32_t current_func_body = 0xFFFFFFFF;
    uint32_t current_arena_func = 0xFFFFFFFF;
    uint32_t current_func_idx = 0xFFFFFFFF;
};

//------------------ struct_shape_key: canonical shape key for a struct local's field/kind/late layout
inline std::string struct_shape_key(const StructLocal &sl) {
    std::string k;
    for (size_t i = 0; i < sl.fields.size(); ++i) {
        if (i)
            k += '|';
        k += sl.fields[i];
        k += ':';
        k += (sl.kinds[i] == FieldKind::Int ? 'i' : (sl.kinds[i] == FieldKind::Double ? 'd' : 's'));
        if (sl.late[i])
            k += '+';
    }
    return k;
}

//------------------ struct_field_kind: kind of a `p.f` read for a promoted struct local, else Unknown
inline FieldKind struct_field_kind(const ASTContext &ctx, const AnalysisState &state, uint32_t node_idx) {
    if (node_idx >= ctx.nodes.size() || ctx.nodes[node_idx].type != NodeType::TableAccess)
        return FieldKind::Unknown;
    const auto &ta = ctx.nodes[node_idx].as.table_access;
    if (ta.table >= ctx.nodes.size() || ctx.nodes[ta.table].type != NodeType::Identifier)
        return FieldKind::Unknown;
    if (ta.key >= ctx.nodes.size() || ctx.nodes[ta.key].type != NodeType::String)
        return FieldKind::Unknown;
    std::string_view tn(ctx.nodes[ta.table].as.ident.name, ctx.nodes[ta.table].as.ident.length);
    auto oit = state.node_func_owner.find(node_idx);
    uint32_t owner = oit != state.node_func_owner.end() ? oit->second : 0xFFFFFFFFu;
    auto it = state.struct_locals.find({ owner, std::string(tn) });
    if (it == state.struct_locals.end())
        return FieldKind::Unknown;
    std::string_view fn(ctx.nodes[ta.key].as.string.text, ctx.nodes[ta.key].as.string.length);
    for (size_t i = 0; i < it->second.fields.size(); ++i) {
        if (it->second.fields[i] == fn) {
            //------------------ an unproven late read may be nil, so it is not a statically known kind
            if (it->second.late[i] && !state.struct_proven_reads.count(node_idx))
                return FieldKind::Unknown;
            return it->second.kinds[i];
        }
    }
    return FieldKind::Unknown;
}

//------------------ builtin_field_ok: static builtin module.field is never written in this file
inline bool builtin_field_ok(const AnalysisState &state, std::string_view module, std::string_view field) {
    if (state.builtin_module_writes.count(std::string(module)))
        return false;
    std::string key(module);
    key += '.';
    key.append(field.data(), field.size());
    return state.builtin_field_writes.count(key) == 0;
}

//------------------ is_purely_integer_expr: returns true if a node always evaluates to an integer
inline bool is_purely_integer_expr(const ASTContext &ctx, const AnalysisState &state, uint32_t node_idx) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto &n = ctx.nodes[node_idx];
    if (n.type == NodeType::Integer)
        return true;
    if (n.type == NodeType::Identifier) {
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        return state.native_integers.count(nm) > 0;
    }
    if (n.type == NodeType::BinaryOp) {
        int bop = n.as.bin_op.op;
        bool is_int_binop = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
            || bop == static_cast<int>(BinaryOp::Mod) || bop == static_cast<int>(BinaryOp::And)
            || bop == static_cast<int>(BinaryOp::Or) || bop == static_cast<int>(BinaryOp::FloorDiv)
            || bop == static_cast<int>(BinaryOp::BitAnd) || bop == static_cast<int>(BinaryOp::BitOr)
            || bop == static_cast<int>(BinaryOp::BitXor) || bop == static_cast<int>(BinaryOp::Shl)
            || bop == static_cast<int>(BinaryOp::Shr);
        if (is_int_binop)
            return is_purely_integer_expr(ctx, state, n.as.bin_op.left)
                && is_purely_integer_expr(ctx, state, n.as.bin_op.right);
    }
    if (n.type == NodeType::UnaryOp
        && (n.as.unary_op.op == static_cast<int>(UnaryOp::Minus)
            || n.as.unary_op.op == static_cast<int>(UnaryOp::BNot)))
        return is_purely_integer_expr(ctx, state, n.as.unary_op.expr);
    if (n.type == NodeType::ParenExpression)
        return is_purely_integer_expr(ctx, state, n.as.paren_expr.expr);
    if (n.type == NodeType::TableAccess) {
        //------------------ math.maxinteger/mininteger are Int64 constants in stock Lua: reading them
        //------------------ never yields a float, so no int-array or struct proof is needed
        if (ctx.nodes[n.as.table_access.table].type == NodeType::Identifier
            && ctx.nodes[n.as.table_access.table].as.ident.is_global
            && ctx.nodes[n.as.table_access.key].type == NodeType::String) {
            std::string_view mn(
                ctx.nodes[n.as.table_access.table].as.ident.name, ctx.nodes[n.as.table_access.table].as.ident.length);
            std::string_view mk(
                ctx.nodes[n.as.table_access.key].as.string.text, ctx.nodes[n.as.table_access.key].as.string.length);
            if (mn == "math" && (mk == "maxinteger" || mk == "mininteger"))
                return true;
        }
        if (struct_field_kind(ctx, state, node_idx) == FieldKind::Int)
            return true;
        if (ctx.nodes[n.as.table_access.table].type == NodeType::Identifier) {
            std::string_view an(
                ctx.nodes[n.as.table_access.table].as.ident.name, ctx.nodes[n.as.table_access.table].as.ident.length);
            return state.int_numeric_arrays.count(an) > 0 && is_purely_integer_expr(ctx, state, n.as.table_access.key);
        }
        return false;
    }
    return false;
}

//------------------ reassigned_with_non_int: true if the variable is assigned a value that is not

inline bool reassigned_with_non_int(
    const ASTContext &ctx, const AnalysisState &state, std::string_view nm, uint32_t decl_idx) {
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto &n = ctx.nodes[ni];
        if (n.type != NodeType::Assignment)
            continue;
        for (uint32_t ti = 0; ti < n.as.assign.target_count; ++ti) {
            uint32_t tgt = ctx.block_statements[n.as.assign.first_target + ti];
            if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier)
                continue;
            std::string_view tn(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (tn != nm || ni == decl_idx)
                continue;
            uint32_t vi
                = (ti < n.as.assign.value_count) ? ctx.block_statements[n.as.assign.first_value + ti] : 0xFFFFFFFF;
            if (!is_purely_integer_expr(ctx, state, vi))
                return true;
        }
    }
    return false;
}

//------------------ is_integer_typed_expr: stricter than is_purely_integer_expr
inline bool is_integer_typed_expr(const ASTContext &ctx, const AnalysisState &state, uint32_t node_idx) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto &n = ctx.nodes[node_idx];
    if (n.type == NodeType::Integer)
        return true;
    if (n.type == NodeType::Identifier) {
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        return state.native_integers.count(nm) > 0;
    }
    if (n.type == NodeType::BinaryOp) {
        int bop = n.as.bin_op.op;
        bool is_int_binop = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
            || bop == static_cast<int>(BinaryOp::Mod) || bop == static_cast<int>(BinaryOp::And)
            || bop == static_cast<int>(BinaryOp::Or) || bop == static_cast<int>(BinaryOp::FloorDiv)
            || bop == static_cast<int>(BinaryOp::BitAnd) || bop == static_cast<int>(BinaryOp::BitOr)
            || bop == static_cast<int>(BinaryOp::BitXor) || bop == static_cast<int>(BinaryOp::Shl)
            || bop == static_cast<int>(BinaryOp::Shr);
        if (is_int_binop)
            return is_integer_typed_expr(ctx, state, n.as.bin_op.left)
                && is_integer_typed_expr(ctx, state, n.as.bin_op.right);
    }
    if (n.type == NodeType::UnaryOp
        && (n.as.unary_op.op == static_cast<int>(UnaryOp::Minus)
            || n.as.unary_op.op == static_cast<int>(UnaryOp::BNot)))
        return is_integer_typed_expr(ctx, state, n.as.unary_op.expr);
    if (n.type == NodeType::ParenExpression)
        return is_integer_typed_expr(ctx, state, n.as.paren_expr.expr);
    if (n.type == NodeType::UnaryOp && n.as.unary_op.op == static_cast<int>(UnaryOp::Len))
        return true;
    if (n.type == NodeType::CallExpression) {
        uint32_t cti = n.as.call_expr.target;
        if (cti < ctx.nodes.size() && ctx.nodes[cti].type == NodeType::Identifier) {
            std::string_view fname(ctx.nodes[cti].as.ident.name, ctx.nodes[cti].as.ident.length);
            if (state.int_returning_funcs.count(fname))
                return true;
        }
        return false;
    }
    if (n.type == NodeType::TableAccess) {
        if (struct_field_kind(ctx, state, node_idx) == FieldKind::Int)
            return true;
        if (ctx.nodes[n.as.table_access.table].type == NodeType::Identifier) {
            std::string_view an(
                ctx.nodes[n.as.table_access.table].as.ident.name, ctx.nodes[n.as.table_access.table].as.ident.length);
            return state.int_numeric_arrays.count(an) > 0 && is_integer_typed_expr(ctx, state, n.as.table_access.key);
        }
        return false;
    }
    return false;
}

//------------------ owner_of_node: node index of the innermost enclosing FunctionDef (0xFFFFFFFF = file scope)
inline uint32_t owner_of_node(const AnalysisState &state, uint32_t node_idx) {
    auto it = state.node_func_owner.find(node_idx);
    return it != state.node_func_owner.end() ? it->second : 0xFFFFFFFFu;
}

}

#endif
