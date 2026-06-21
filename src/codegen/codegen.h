// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  codegen.h · CodeGen Header                 │
// └─────────────────────────────────────────────┘

#ifndef CODEGEN_H
#define CODEGEN_H

#include "../syntax/nodes.h"
#include <fstream>
#include <string_view>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>

namespace clx {


//------------------ LocalVar: tracks a local variable's name and boxed status
struct LocalVar {
    std::string_view name;
    bool is_boxed;
};


//------------------ CodeEmitter: generates C++ source from the AST
//------------------ lookup_builtin: maps "module.func" to C++ function name
const char* lookup_builtin(std::string_view module, std::string_view func);

class CodeEmitter {
public:
    //------------------ CodeEmitter: constructs emitter for a given AST context
    CodeEmitter(const ASTContext& context, const char* output_path);
    //------------------ emit: generates C++ code for the AST rooted at root_node
    void emit(uint32_t root_node, std::string_view module_name);

private:
    const ASTContext& ctx;
    std::ofstream out;
    std::vector<LocalVar> locals;

    //------------------ is_local: checks if name is a local and sets out_is_boxed
    bool is_local(std::string_view name, bool& out_is_boxed);
    //------------------ emit_node: emits C++ for a single AST node
    void emit_node(uint32_t node_idx);

public:
    //------------------ Optimization analysis state (populated before codegen)
    static std::vector<std::string_view> g_native_numbers;
    static std::vector<std::string_view> g_string_pool;
    static std::set<std::string_view> g_native_return_funcs;
    static std::set<std::string> g_param_numbers;
    static std::vector<std::string> g_param_names;
    static std::map<uint32_t, uint32_t> g_table_presize;
    static std::map<std::string_view, double> g_global_constants;
    static std::set<uint32_t> g_bce_safe_nodes;
    static std::set<std::string_view> g_pure_numeric_arrays;
    static std::set<std::string_view> g_native_integers;
    //------------------ Per-function analysis data
    static std::map<std::string_view, std::set<std::string_view>> g_numeric_table_fields;
    static std::set<std::string_view> g_direct_callables;
    static std::set<std::string_view> g_fast_callables;
    static std::map<std::string_view, uint32_t> g_func_param_counts;
    static std::map<std::string_view, std::vector<bool>> g_func_param_native;
    static std::set<std::string_view> g_reassigned_vars;
    static std::set<std::string_view> g_constant_upvalues;
    static std::set<std::string_view> g_string_builders;
    static std::set<std::string_view> g_global_string_builders;
    static std::set<std::string_view> g_module_string_builders;
    static std::map<uint32_t, uint32_t> g_goto_targets;
    static std::set<std::string_view> g_hoisted_locals;
    static bool g_skip_block_braces;
    static std::unordered_map<uint32_t, std::string> g_hoisted_lookups;
    static std::unordered_map<std::string, std::string> g_hoisted_cfuncs;
    static std::unordered_map<std::string, std::string> g_builtin_aliases;

    //------------------ Codegen emission state flags
    static bool g_emit_raw_lambda;
    static bool g_emit_fast_lambda;
    static bool g_in_function_def;
    static bool g_in_fast_function;
    static bool g_expect_multivalue;
    static std::string_view g_current_fast_func;
    static std::string g_ref_capture;
    static int g_cs_index;
    static constexpr int g_cs_max = 4;
};

}

#endif