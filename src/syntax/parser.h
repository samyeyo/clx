// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  parser.h · Parser Header                   │
// └─────────────────────────────────────────────┘

#ifndef SYNTAX_PARSER_H
#define SYNTAX_PARSER_H
#include "nodes.h"
#include "lexer.h"
#include <vector>
#include <string_view>

namespace clx {
//------------------ Parser: recursive-descent parser that builds the AST
class Parser {
public:
    //------------------ Parser: constructs parser from source, filename, and AST context
    Parser(const char *source, const char *filename, ASTContext &context);
    //------------------ parse: parses the full source and returns the root block node
    uint32_t parse();

private:
    Lexer lexer;
    ASTContext &ctx;
    Token current_token;
    std::vector<ImplicitGlobalMode> implicit_globals;
    int current_depth = 0;
    int current_function_depth = 0;
    std::vector<Symbol> active_symbols;
    //------------------ advance: consume the current token and fetch the next
    void advance();
    //------------------ add_node: allocate an AST node and return its index
    uint32_t add_node(const ASTNode &node);
    //------------------ parse_statement: parse one complete statement
    uint32_t parse_statement();
    //------------------ parse_expression: parse an expression (lowest precedence)
    uint32_t parse_expression();
    //------------------ parse_term: parse a term (logical or)
    uint32_t parse_term();
    //------------------ parse_factor: parse a factor (logical and)
    uint32_t parse_factor();
    //------------------ parse_primary: parse a primary expression
    uint32_t parse_primary();
    //------------------ parse_postfix_expression: parse postfix operators (calls, indices)
    uint32_t parse_postfix_expression();
    //------------------ parse_block: parse a block of statements
    uint32_t parse_block(bool is_main);
    //------------------ parse_funcbody: parse function parameter list and body
    uint32_t parse_funcbody(bool is_method);
    //------------------ parse_relational: parse relational operators (<, >, ==, etc)
    uint32_t parse_relational();
    //------------------ parse_logical_and: parse logical and
    uint32_t parse_logical_and();
    //------------------ parse_concat: parse concatenation operator
    uint32_t parse_concat();
    //------------------ parse_pow: parse power operator
    uint32_t parse_pow();
    //------------------ parse_unary: parse unary operators
    uint32_t parse_unary();
    //------------------ parse_bitwise_or: parse bitwise or
    uint32_t parse_bitwise_or();
    //------------------ parse_bitwise_xor: parse bitwise xor
    uint32_t parse_bitwise_xor();
    //------------------ parse_bitwise_and: parse bitwise and
    uint32_t parse_bitwise_and();
    //------------------ parse_shift: parse shift operators
    uint32_t parse_shift();
    //------------------ enter_scope: push a new local scope
    void enter_scope();
    //------------------ leave_scope: pop the current local scope
    void leave_scope();
    //------------------ add_symbol: register a variable in the current scope
    void add_symbol(std::string_view name, SymbolType type, uint32_t decl_idx);
    //------------------ resolve_symbol: look up a variable name in active scopes
    SymbolType resolve_symbol(std::string_view name, int line, bool &out_is_captured, bool &out_is_global);
    static constexpr uint32_t INVALID_NODE = 0xFFFFFFFF;
};
}
#endif