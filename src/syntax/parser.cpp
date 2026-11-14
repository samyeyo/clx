// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  parser.cpp · Lua Parser                    │
// └─────────────────────────────────────────────┘

#include "parser.h"
#include "../codegen/codegen.h"
#include "../codegen/helpers.h"
#include <charconv>
#include <stdexcept>

#define INVALID_NODE 0xFFFFFFFF

namespace clx {

static constexpr const char *std_libs[] = { "print", "require", "error", "assert", "tostring", "tonumber", "type",
    "pairs", "ipairs", "next", "pcall", "xpcall", "setmetatable", "getmetatable", "rawget", "rawset", "rawlen",
    "rawequal", "collectgarbage", "load", "loadfile", "_G", "_VERSION", "warn", "math", "table", "string", "os",
    "coroutine", "package", "debug", "io", "utf8" };

//------------------ parser_parse_numeral: Lua numeral -> int64, false when the text is not an integer constant
//------------------ (hex wraps modulo 2^64 as in l_str2int, decimal overflows fall through to a float)
static bool parser_parse_numeral(std::string_view text, int64_t &out) {
    size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
        i++;
    bool is_hex = (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X'));
    if (is_hex)
        i += 2;
    if (i >= text.size())
        return false;
    uint64_t acc = 0;
    for (; i < text.size(); i++) {
        char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<unsigned>(c - '0');
        else if (is_hex && c >= 'a' && c <= 'f')
            digit = static_cast<unsigned>(c - 'a' + 10);
        else if (is_hex && c >= 'A' && c <= 'F')
            digit = static_cast<unsigned>(c - 'A' + 10);
        else
            return false;
        if (is_hex) {
            acc = acc * 16u + digit;
        } else {
            if (acc > (UINT64_MAX - digit) / 10u)
                return false;
            acc = acc * 10u + digit;
            if (acc > static_cast<uint64_t>(INT64_MAX))
                return false;
        }
    }
    out = static_cast<int64_t>(acc);
    return true;
}

//------------------ PARSER: constructor - initializes parser with lexer and AST context, seeds std lib symbols
Parser::Parser(const char *source, const char *filename, ASTContext &context)
    : lexer(source, filename)
    , ctx(context) {
    ctx.filename = filename ? filename : "";
    current_token = lexer.current();
    implicit_globals.push_back(ImplicitGlobalMode::Default);

    for (const char *lib : std_libs) {
        active_symbols.push_back({ lib, SymbolType::ExplicitGlobal, 0, 0, 0xFFFFFFFF });
    }
}

//------------------ PARSER: advance - advances lexer and updates current token
void Parser::advance() {
    lexer.advance();
    current_token = lexer.current();
}

//------------------ PARSER: add_node - appends an AST node and returns its index
uint32_t Parser::add_node(const ASTNode &node) {
    ctx.nodes.push_back(node);
    return static_cast<uint32_t>(ctx.nodes.size() - 1);
}

//------------------ PARSER: enter_scope - increments depth, pushes inherited implicit_globals
void Parser::enter_scope() {
    current_depth++;
    implicit_globals.push_back(implicit_globals.back());
}

//------------------ PARSER: leave_scope - pops symbols for current depth and decrements depth
void Parser::leave_scope() {
    while (!active_symbols.empty() && active_symbols.back().depth == current_depth) {
        active_symbols.pop_back();
    }
    current_depth--;
    implicit_globals.pop_back();
}

//------------------ PARSER: add_symbol - registers a symbol in the active scope
void Parser::add_symbol(std::string_view name, SymbolType type, uint32_t decl_idx) {
    active_symbols.push_back({ name, type, current_depth, current_function_depth, decl_idx });
}

//------------------ PARSER: resolve_symbol - looks up a symbol in scope chain, returns its type and flags
SymbolType Parser::resolve_symbol(std::string_view name, int line, bool &out_is_captured, bool &out_is_global) {
    out_is_captured = false;
    out_is_global = false;
    for (auto it = active_symbols.rbegin(); it != active_symbols.rend(); ++it) {
        if (it->name == name) {
            if (it->function_depth > current_function_depth)
                continue;
            if (it->type == SymbolType::ExplicitGlobal || it->type == SymbolType::ConstGlobal) {
                out_is_global = true;
                return it->type;
            }
            if (it->function_depth < current_function_depth) {
                ctx.nodes[it->decl_idx].as.ident.is_captured = true;
                out_is_captured = true;
            }
            return it->type;
        }
    }
    //------------------ _ENV is an implicit outer local of every chunk, so it is never a free name
    if (name == "_ENV") {
        out_is_global = true;
        return SymbolType::ExplicitGlobal;
    }
    if (implicit_globals.back() == ImplicitGlobalMode::None) {
        throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(line)
            + ": use of undeclared global variable '" + std::string(name) + "'");
    }
    out_is_global = true;
    return (implicit_globals.back() == ImplicitGlobalMode::ReadOnly) ? SymbolType::ConstGlobal
                                                                     : SymbolType::ExplicitGlobal;
}

//------------------ PARSER: parse_primary - parses primary expressions (literals, identifiers, table/paren/func)
uint32_t Parser::parse_primary() {
    if (current_token.type == TokenType::TokLBrace) {
        int line = current_token.line;
        advance();
        std::vector<uint32_t> keys;
        std::vector<uint32_t> values;

        while (current_token.type != TokenType::TokRBrace && current_token.type != TokenType::TokEof) {
            bool has_key = false;
            if (current_token.type == TokenType::TokIdent) {
                std::string_view rest = lexer.remaining_source();
                size_t check_pos = 0;
                while (check_pos < rest.size()
                    && (rest[check_pos] == ' ' || rest[check_pos] == '\t' || rest[check_pos] == '\n'
                        || rest[check_pos] == '\r'))
                    check_pos++;
                bool is_key_value = (check_pos < rest.size() && rest[check_pos] == '=');
                if (is_key_value) {
                    ASTNode k;
                    k.type = NodeType::String;
                    k.line = current_token.line;
                    k.as.string.text = current_token.text.data();
                    k.as.string.length = current_token.text.length();
                    keys.push_back(add_node(k));
                    advance();
                    advance();
                    has_key = true;
                }
            } else if (current_token.type == TokenType::TokLBracket) {
                advance();
                keys.push_back(parse_expression());
                if (current_token.type == TokenType::TokRBracket)
                    advance();
                if (current_token.type == TokenType::TokAssign)
                    advance();
                has_key = true;
            }

            if (!has_key)
                keys.push_back(INVALID_NODE);

            uint32_t val = parse_expression();
            if (val == INVALID_NODE)
                throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                    + ": unexpected token in table constructor");
            values.push_back(val);

            if (current_token.type == TokenType::TokComma || current_token.type == TokenType::TokSemicolon)
                advance();
            else if (current_token.type != TokenType::TokRBrace) {
                throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                    + ": expected ',' or '}' in table");
            }
        }
        if (current_token.type == TokenType::TokRBrace)
            advance();

        ASTNode node;
        node.type = NodeType::TableConstructor;
        node.line = line;
        node.as.table_cons.count = (uint32_t)values.size();
        node.as.table_cons.first_item = (uint32_t)ctx.block_statements.size();

        for (size_t i = 0; i < values.size(); ++i) {
            ctx.block_statements.push_back(keys[i]);
            ctx.block_statements.push_back(values[i]);
        }
        return add_node(node);
    }
    if (current_token.type == TokenType::TokVararg) {
        ASTNode node;
        node.type = NodeType::Vararg;
        node.line = current_token.line;
        advance();
        return add_node(node);
    }
    if (current_token.type == TokenType::TokLParen) {
        int line = current_token.line;
        advance();
        uint32_t expr = parse_expression();
        if (current_token.type == TokenType::TokRParen)
            advance();

        ASTNode node;
        node.type = NodeType::ParenExpression;
        node.line = line;
        node.as.paren_expr.expr = expr;
        return add_node(node);
    }
    if (current_token.type == TokenType::TokNumber) {
        ASTNode node;
        node.line = current_token.line;
        int64_t ival = 0;
        if (parser_parse_numeral(current_token.text, ival)) {
            node.type = NodeType::Integer;
            node.as.integer.val = ival;
        } else {
            node.type = NodeType::Number;
            node.as.number.val = current_token.number_value;
        }
        advance();
        return add_node(node);
    }
    if (current_token.type == TokenType::TokString) {
        ASTNode node;
        node.type = NodeType::String;
        node.line = current_token.line;
        if (current_token.raw) {
            node.as.string.text = current_token.text.data();
            node.as.string.length = current_token.text.length();
        } else {
            std::string decoded;
            try {
                decoded = lua_decode_string(current_token.text);
            } catch (const std::exception &e) {
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": " + e.what());
            }
            node.as.string.length = decoded.length();
            ctx.string_storage.push_back(std::move(decoded));
            node.as.string.text = ctx.string_storage.back().data();
        }
        advance();
        return add_node(node);
    }
    if (current_token.type == TokenType::TokTrue || current_token.type == TokenType::TokFalse || current_token.type == TokenType::TokNil) {
        ASTNode node;
        node.line = current_token.line;
        if (current_token.type == TokenType::TokTrue)
            node.type = NodeType::TrueLiteral;
        else if (current_token.type == TokenType::TokFalse)
            node.type = NodeType::FalseLiteral;
        else
            node.type = NodeType::NilLiteral;
        advance();
        return add_node(node);
    }
    if (current_token.type == TokenType::TokFunction) {
        advance();
        return parse_funcbody(false);
    }
    if (current_token.type == TokenType::TokIdent) {
        ASTNode node;
        node.type = NodeType::Identifier;
        node.line = current_token.line;
        node.as.ident.name = current_token.text.data();
        node.as.ident.length = current_token.text.length();
        node.as.ident.is_captured = false;
        node.as.ident.is_global = false;
        node.as.ident.attr = Attribute::None;
        uint32_t node_idx = add_node(node);

        bool is_cap = false, is_glob = false;
        resolve_symbol(current_token.text, current_token.line, is_cap, is_glob);
        ctx.nodes[node_idx].as.ident.is_captured = is_cap;
        ctx.nodes[node_idx].as.ident.is_global = is_glob;

        advance();
        return node_idx;
    }
    return INVALID_NODE;
}

//------------------ PARSER: parse_postfix_expression - parses method calls, index access, function calls
uint32_t Parser::parse_postfix_expression() {
    uint32_t expr = parse_primary();
    //------------------ stock's suffixedexp only extends a NAME or a parenthesized expression: `{}.x` and
    //------------------ `function() end()` are syntax errors, and a constructor followed by `(` on the next
    //------------------ line starts a new statement instead of calling the constructor
    if (expr < ctx.nodes.size()) {
        NodeType base_type = ctx.nodes[expr].type;
        if (base_type != NodeType::Identifier && base_type != NodeType::ParenExpression) {
            switch (current_token.type) {
            case TokenType::TokString:
            case TokenType::TokLBrace:
            case TokenType::TokDot:
            case TokenType::TokLBracket:
            case TokenType::TokColon:
                throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                    + ": unexpected symbol near '" + std::string(current_token.text) + "'");
            default:
                break;
            }
            return expr;
        }
    }
    while (true) {
        if (current_token.type == TokenType::TokString || current_token.type == TokenType::TokLBrace) {
            int line = current_token.line;
            uint32_t arg = parse_primary();
            uint32_t first_arg = ctx.block_statements.size();
            ctx.block_statements.push_back(arg);
            ASTNode node;
            node.type = NodeType::CallExpression;
            node.line = line;
            node.as.call_expr.target = expr;
            node.as.call_expr.first_arg = first_arg;
            node.as.call_expr.arg_count = 1;
            expr = add_node(node);
        } else if (current_token.type == TokenType::TokLParen) {
            int line = current_token.line;
            advance();
            std::vector<uint32_t> args;
            if (current_token.type != TokenType::TokRParen) {
                args.push_back(parse_expression());
                while (current_token.type == TokenType::TokComma) {
                    advance();
                    args.push_back(parse_expression());
                }
            }
            if (current_token.type == TokenType::TokRParen)
                advance();

            uint32_t first_arg = ctx.block_statements.size();
            for (uint32_t arg : args)
                ctx.block_statements.push_back(arg);

            //------------------ direct module intrinsic rewrite: registry entry on a non-shadowed
            //------------------ global table with enough args keeps generic dispatch below min_args
            const char *intrinsic_cname = nullptr;
            if (ctx.nodes[expr].type == NodeType::TableAccess) {
                uint32_t t_idx = ctx.nodes[expr].as.table_access.table;
                uint32_t k_idx = ctx.nodes[expr].as.table_access.key;
                if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[t_idx].as.ident.is_global
                    && ctx.nodes[k_idx].type == NodeType::String) {
                    std::string_view tname(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                    std::string_view kname(ctx.nodes[k_idx].as.string.text, ctx.nodes[k_idx].as.string.length);
                    const BuiltinEntry *be = builtin_lookup(tname, kname);
                    if (be && be->intrinsic && args.size() >= be->min_args)
                        intrinsic_cname = be->intrinsic;
                }
            } else if (ctx.nodes[expr].type == NodeType::Identifier && ctx.nodes[expr].as.ident.is_global) {
                std::string_view fname(ctx.nodes[expr].as.ident.name, ctx.nodes[expr].as.ident.length);
                const BuiltinEntry *be = builtin_lookup("_G", fname);
                if (be && be->intrinsic && args.size() >= be->min_args)
                    intrinsic_cname = be->intrinsic;
            }

            ASTNode node;
            if (intrinsic_cname) {
                node.type = NodeType::IntrinsicCall;
                node.line = line;
                node.as.intrinsic_call.cname = intrinsic_cname;
                node.as.intrinsic_call.first_arg = first_arg;
                node.as.intrinsic_call.arg_count = args.size();
            } else {
                node.type = NodeType::CallExpression;
                node.line = line;
                node.as.call_expr.target = expr;
                node.as.call_expr.first_arg = first_arg;
                node.as.call_expr.arg_count = args.size();
            }
            expr = add_node(node);
        } else if (current_token.type == TokenType::TokColon) {
            int line = current_token.line;
            advance();
            if (current_token.type != TokenType::TokIdent)
                throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                    + ": Expected method name after ':'");

            ASTNode key;
            key.type = NodeType::String;
            key.line = current_token.line;
            key.as.string.text = current_token.text.data();
            key.as.string.length = current_token.text.length();
            uint32_t key_idx = add_node(key);
            advance();

            ASTNode access_node;
            access_node.type = NodeType::TableAccess;
            access_node.line = line;
            access_node.as.table_access.table = expr;
            access_node.as.table_access.key = key_idx;
            uint32_t target_func = add_node(access_node);

            if (current_token.type == TokenType::TokString || current_token.type == TokenType::TokLBrace) {
                uint32_t arg = parse_primary();
                std::vector<uint32_t> args;
                args.push_back(expr);
                args.push_back(arg);
                uint32_t first_arg = ctx.block_statements.size();
                for (uint32_t a : args)
                    ctx.block_statements.push_back(a);
                ASTNode node;
                node.type = NodeType::CallExpression;
                node.line = line;
                node.as.call_expr.target = target_func;
                node.as.call_expr.first_arg = first_arg;
                node.as.call_expr.arg_count = args.size();
                expr = add_node(node);
            } else {
                if (current_token.type != TokenType::TokLParen)
                    throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                        + ": Expected '(' for method call");
                advance();

                std::vector<uint32_t> args;
                args.push_back(expr);

                if (current_token.type != TokenType::TokRParen) {
                    args.push_back(parse_expression());
                    while (current_token.type == TokenType::TokComma) {
                        advance();
                        args.push_back(parse_expression());
                    }
                }
                if (current_token.type == TokenType::TokRParen)
                    advance();

                uint32_t first_arg = ctx.block_statements.size();
                for (uint32_t arg : args)
                    ctx.block_statements.push_back(arg);

                ASTNode node;
                node.type = NodeType::CallExpression;
                node.line = line;
                node.as.call_expr.target = target_func;
                node.as.call_expr.first_arg = first_arg;
                node.as.call_expr.arg_count = args.size();
                expr = add_node(node);
            }
        } else if (current_token.type == TokenType::TokDot) {
            advance();
            ASTNode key;
            key.type = NodeType::String;
            key.as.string.text = current_token.text.data();
            key.as.string.length = current_token.text.length();
            uint32_t key_idx = add_node(key);
            advance();
            ASTNode node;
            node.type = NodeType::TableAccess;
            node.as.table_access.table = expr;
            node.as.table_access.key = key_idx;
            expr = add_node(node);
        } else if (current_token.type == TokenType::TokLBracket) {
            advance();
            uint32_t key_idx = parse_expression();
            if (current_token.type == TokenType::TokRBracket)
                advance();
            ASTNode node;
            node.type = NodeType::TableAccess;
            node.as.table_access.table = expr;
            node.as.table_access.key = key_idx;
            expr = add_node(node);
        } else
            break;
    }
    return expr;
}

//------------------ PARSER: parse_unary - parses unary operators (-, #, ~, not)
uint32_t Parser::parse_unary() {
    if (current_token.type == TokenType::TokMinus || current_token.type == TokenType::TokLen || current_token.type == TokenType::TokBitXor
        || current_token.type == TokenType::TokNot) {
        int op_code = static_cast<int>(UnaryOp::Len);
        if (current_token.type == TokenType::TokMinus)
            op_code = static_cast<int>(UnaryOp::Minus);
        if (current_token.type == TokenType::TokBitXor)
            op_code = static_cast<int>(UnaryOp::BNot);
        if (current_token.type == TokenType::TokNot)
            op_code = static_cast<int>(UnaryOp::Not);
        int line = current_token.line;
        advance();
        uint32_t expr = parse_unary();
        ASTNode node;
        node.type = NodeType::UnaryOp;
        node.line = line;
        node.as.unary_op.expr = expr;
        node.as.unary_op.op = op_code;
        return add_node(node);
    }
    return parse_pow();
}

//------------------ PARSER: parse_pow - parses power operator (^)
uint32_t Parser::parse_pow() {
    uint32_t left = parse_postfix_expression();
    if (current_token.type == TokenType::TokPow) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_unary();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::Pow);
        return add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_factor - parses multiplicative operators (*, /, %, //)
uint32_t Parser::parse_factor() {
    uint32_t left = parse_unary();
    while (current_token.type == TokenType::TokStar || current_token.type == TokenType::TokSlash || current_token.type == TokenType::TokMod
        || current_token.type == TokenType::TokFloorDiv) {
        int line = current_token.line;
        int op_code = static_cast<int>(BinaryOp::Mul);
        if (current_token.type == TokenType::TokSlash)
            op_code = static_cast<int>(BinaryOp::Div);
        else if (current_token.type == TokenType::TokMod)
            op_code = static_cast<int>(BinaryOp::Mod);
        else if (current_token.type == TokenType::TokFloorDiv)
            op_code = static_cast<int>(BinaryOp::FloorDiv);
        advance();
        uint32_t right = parse_unary();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = op_code;
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_term - parses additive operators (+, -)
uint32_t Parser::parse_term() {
    uint32_t left = parse_factor();
    while (current_token.type == TokenType::TokPlus || current_token.type == TokenType::TokMinus) {
        int line = current_token.line;
        int op_code
            = (current_token.type == TokenType::TokPlus) ? static_cast<int>(BinaryOp::Add) : static_cast<int>(BinaryOp::Sub);
        advance();
        uint32_t right = parse_factor();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = op_code;
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_bitwise_or - parses bitwise OR operator (|)
uint32_t Parser::parse_bitwise_or() {
    uint32_t left = parse_bitwise_xor();
    while (current_token.type == TokenType::TokBitOr) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_bitwise_xor();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::BitOr);
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_bitwise_xor - parses bitwise XOR operator (~)
uint32_t Parser::parse_bitwise_xor() {
    uint32_t left = parse_bitwise_and();
    while (current_token.type == TokenType::TokBitXor) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_bitwise_and();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::BitXor);
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_bitwise_and - parses bitwise AND operator (&)
uint32_t Parser::parse_bitwise_and() {
    uint32_t left = parse_shift();
    while (current_token.type == TokenType::TokBitAnd) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_shift();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::BitAnd);
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_shift - parses shift operators (<<, >>)
uint32_t Parser::parse_shift() {
    uint32_t left = parse_concat();
    while (current_token.type == TokenType::TokShl || current_token.type == TokenType::TokShr) {
        int line = current_token.line;
        int op = (current_token.type == TokenType::TokShl) ? static_cast<int>(BinaryOp::Shl) : static_cast<int>(BinaryOp::Shr);
        advance();
        uint32_t right = parse_concat();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = op;
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_concat - parses string concatenation operator (..)
uint32_t Parser::parse_concat() {
    uint32_t left = parse_term();
    if (current_token.type == TokenType::TokConcat) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_concat();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::Concat);
        return add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_relational - parses comparison operators (==, <, >, <=, >=, ~=)
uint32_t Parser::parse_relational() {
    uint32_t left = parse_bitwise_or();
    while (current_token.type == TokenType::TokEqEq || current_token.type == TokenType::TokNotEq || current_token.type == TokenType::TokLess
        || current_token.type == TokenType::TokGreater || current_token.type == TokenType::TokLessEq || current_token.type == TokenType::TokGreaterEq) {
        int line = current_token.line;
        int op_code = 0;
        if (current_token.type == TokenType::TokEqEq)
            op_code = static_cast<int>(BinaryOp::Eq);
        else if (current_token.type == TokenType::TokLess)
            op_code = static_cast<int>(BinaryOp::Lt);
        else if (current_token.type == TokenType::TokGreater)
            op_code = static_cast<int>(BinaryOp::Gt);
        else if (current_token.type == TokenType::TokLessEq)
            op_code = static_cast<int>(BinaryOp::Le);
        else if (current_token.type == TokenType::TokGreaterEq)
            op_code = static_cast<int>(BinaryOp::Ge);
        else if (current_token.type == TokenType::TokNotEq)
            op_code = static_cast<int>(BinaryOp::Ne);
        advance();
        uint32_t right = parse_bitwise_or();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = op_code;
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_logical_and - parses logical AND operator
uint32_t Parser::parse_logical_and() {
    uint32_t left = parse_relational();
    while (current_token.type == TokenType::TokAnd) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_relational();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::And);
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_expression - parses logical OR (lowest precedence operator)
uint32_t Parser::parse_expression() {
    uint32_t left = parse_logical_and();
    while (current_token.type == TokenType::TokOr) {
        int line = current_token.line;
        advance();
        uint32_t right = parse_logical_and();
        ASTNode node;
        node.type = NodeType::BinaryOp;
        node.line = line;
        node.as.bin_op.left = left;
        node.as.bin_op.right = right;
        node.as.bin_op.op = static_cast<int>(BinaryOp::Or);
        left = add_node(node);
    }
    return left;
}

//------------------ PARSER: parse_statement - dispatches on current token to parse a single statement
uint32_t Parser::parse_statement() {
    //------------------ PARSE: stmt - end of file, return invalid
    if (current_token.type == TokenType::TokEof)
        return INVALID_NODE;
    //------------------ PARSE: stmt - empty statement (semicolon), skip
    if (current_token.type == TokenType::TokSemicolon) {
        advance();
        return INVALID_NODE;
    }
    //------------------ PARSE: stmt - label statement (::label::)
    if (current_token.type == TokenType::TokDoubleColon) {
        int line = current_token.line;
        advance();

        if (current_token.type != TokenType::TokIdent)
            throw std::runtime_error(
                "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": Expected label name");
        ASTNode name_node;
        name_node.type = NodeType::Identifier;
        name_node.line = current_token.line;
        name_node.as.ident.name = current_token.text.data();
        name_node.as.ident.length = current_token.text.length();
        uint32_t name_idx = add_node(name_node);
        advance();

        if (current_token.type != TokenType::TokDoubleColon)
            throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                + ": Expected '::' after label name");
        advance();

        ASTNode node;
        node.type = NodeType::LabelStatement;
        node.line = line;
        node.as.label_stmt.name_ident = name_idx;
        return add_node(node);
    }
    //------------------ PARSE: stmt - goto statement
    if (current_token.type == TokenType::TokGoto) {
        int line = current_token.line;
        advance();

        if (current_token.type != TokenType::TokIdent)
            throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                + ": Expected label name after 'goto'");
        ASTNode name_node;
        name_node.type = NodeType::Identifier;
        name_node.line = current_token.line;
        name_node.as.ident.name = current_token.text.data();
        name_node.as.ident.length = current_token.text.length();
        uint32_t name_idx = add_node(name_node);
        advance();

        ASTNode node;
        node.type = NodeType::GotoStatement;
        node.line = line;
        node.as.goto_stmt.name_ident = name_idx;
        return add_node(node);
    }
    //------------------ PARSE: stmt - global declaration (function, variable, or wildcard)
    if (current_token.type == TokenType::TokGlobal) {
        int line = current_token.line;
        advance();

        if (current_token.type == TokenType::TokFunction) {

            if (implicit_globals.back() == ImplicitGlobalMode::Default)
                implicit_globals.back() = ImplicitGlobalMode::None;
            advance();
            if (current_token.type != TokenType::TokIdent)
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": Expected function name");

            ASTNode ident_node;
            ident_node.type = NodeType::Identifier;
            ident_node.line = current_token.line;
            ident_node.as.ident.name = current_token.text.data();
            ident_node.as.ident.length = current_token.text.length();
            ident_node.as.ident.is_captured = false;
            ident_node.as.ident.is_global = true;
            ident_node.as.ident.attr = Attribute::None;
            uint32_t target = add_node(ident_node);

            add_symbol(current_token.text, SymbolType::ExplicitGlobal, target);
            advance();

            uint32_t func_expr = parse_funcbody(false);

            ctx.block_statements.push_back(target);
            ctx.block_statements.push_back(func_expr);

            ASTNode node;
            node.type = NodeType::GlobalDeclStatement;
            node.line = line;
            node.as.global_decl.first_ident = ctx.block_statements.size() - 2;
            node.as.global_decl.ident_count = 1;
            node.as.global_decl.first_value = ctx.block_statements.size() - 1;
            node.as.global_decl.value_count = 1;
            node.as.global_decl.is_wildcard = false;
            return add_node(node);
        }

        Attribute attr = Attribute::None;
        if (current_token.type == TokenType::TokLess) {
            advance();
            if (current_token.type == TokenType::TokIdent && current_token.text == "const") {
                attr = Attribute::Const;
            } else {
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": invalid attribute");
            }
            advance();
            if (current_token.type != TokenType::TokGreater)
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": expected '>'");
            advance();
        }

        if (current_token.type == TokenType::TokStar) {
            implicit_globals.back()
                = (attr == Attribute::Const) ? ImplicitGlobalMode::ReadOnly : ImplicitGlobalMode::ReadWrite;
            advance();
            ASTNode node;
            node.type = NodeType::GlobalDeclStatement;
            node.line = line;
            node.as.global_decl.is_wildcard = true;
            node.as.global_decl.ident_count = 0;
            node.as.global_decl.value_count = 0;
            return add_node(node);
        }

        {
            auto saved_mode = implicit_globals.back();
            implicit_globals.back() = ImplicitGlobalMode::None;

            std::vector<uint32_t> idents;
            //------------------ the names enter scope only after the initializer is parsed (stock
            //------------------ globalnames bumps nactvar afterwards), so `global a = a` reads the
            //------------------ enclosing binding for the right-hand side
            std::vector<std::pair<std::string_view, SymbolType>> pending_symbols;
            std::vector<uint32_t> pending_nodes;
            do {
                if (current_token.type != TokenType::TokIdent)
                    throw std::runtime_error(
                        "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": Identifier expected");

                ASTNode ident_node;
                ident_node.type = NodeType::Identifier;
                ident_node.line = current_token.line;
                ident_node.as.ident.name = current_token.text.data();
                ident_node.as.ident.length = current_token.text.length();
                ident_node.as.ident.is_captured = false;
                ident_node.as.ident.is_global = true;
                ident_node.as.ident.attr = attr;

                std::string_view name = current_token.text;
                advance();

                Attribute local_attr = attr;
                if (current_token.type == TokenType::TokLess) {
                    advance();
                    if (current_token.type == TokenType::TokIdent && current_token.text == "const")
                        local_attr = Attribute::Const;
                    advance();
                    advance();
                }
                ident_node.as.ident.attr = local_attr;

                uint32_t id_idx = add_node(ident_node);
                idents.push_back(id_idx);

                SymbolType sym_type
                    = (local_attr == Attribute::Const) ? SymbolType::ConstGlobal : SymbolType::ExplicitGlobal;
                pending_symbols.push_back({ name, sym_type });
                pending_nodes.push_back(id_idx);

            } while (current_token.type == TokenType::TokComma && (advance(), true));

            std::vector<uint32_t> values;
            if (current_token.type == TokenType::TokAssign) {
                advance();
                do {
                    values.push_back(parse_expression());
                } while (current_token.type == TokenType::TokComma && (advance(), true));
            }

            for (size_t _si = 0; _si < pending_symbols.size(); ++_si)
                add_symbol(pending_symbols[_si].first, pending_symbols[_si].second, pending_nodes[_si]);

            uint32_t first_id = ctx.block_statements.size();
            for (auto id : idents)
                ctx.block_statements.push_back(id);
            uint32_t first_val = ctx.block_statements.size();
            for (auto v : values)
                ctx.block_statements.push_back(v);

            implicit_globals.back()
                = (saved_mode == ImplicitGlobalMode::Default) ? ImplicitGlobalMode::None : saved_mode;

            ASTNode node;
            node.type = NodeType::GlobalDeclStatement;
            node.line = line;
            node.as.global_decl.first_ident = first_id;
            node.as.global_decl.ident_count = static_cast<uint32_t>(idents.size());
            node.as.global_decl.first_value = first_val;
            node.as.global_decl.value_count = static_cast<uint32_t>(values.size());
            node.as.global_decl.is_wildcard = false;
            return add_node(node);
        }
    }
    //------------------ PARSE: stmt - function declaration statement (assignment to name)
    if (current_token.type == TokenType::TokFunction) {
        int line = current_token.line;
        advance();

        uint32_t target = INVALID_NODE;
        if (current_token.type == TokenType::TokIdent) {
            ASTNode id_node;
            id_node.type = NodeType::Identifier;
            id_node.line = current_token.line;
            id_node.as.ident.name = current_token.text.data();
            id_node.as.ident.length = current_token.text.length();
            id_node.as.ident.is_captured = false;
            id_node.as.ident.is_global = false;
            id_node.as.ident.attr = Attribute::None;
            target = add_node(id_node);

            bool is_cap, is_glob;
            resolve_symbol(current_token.text, current_token.line, is_cap, is_glob);
            ctx.nodes[target].as.ident.is_captured = is_cap;
            ctx.nodes[target].as.ident.is_global = is_glob;

            advance();

            while (current_token.type == TokenType::TokDot) {
                advance();
                if (current_token.type != TokenType::TokIdent)
                    throw std::runtime_error("Expected identifier after '.'");
                ASTNode key_node;
                key_node.type = NodeType::String;
                key_node.line = current_token.line;
                key_node.as.string.text = current_token.text.data();
                key_node.as.string.length = current_token.text.length();
                uint32_t key_idx = add_node(key_node);
                advance();

                ASTNode access_node;
                access_node.type = NodeType::TableAccess;
                access_node.line = current_token.line;
                access_node.as.table_access.table = target;
                access_node.as.table_access.key = key_idx;
                target = add_node(access_node);
            }
        } else {
            throw std::runtime_error("Expected function name");
        }

        bool is_method = false;
        if (current_token.type == TokenType::TokColon) {
            is_method = true;
            advance();
            if (current_token.type != TokenType::TokIdent)
                throw std::runtime_error("Expected method name after ':'");
            ASTNode key_node;
            key_node.type = NodeType::String;
            key_node.line = current_token.line;
            key_node.as.string.text = current_token.text.data();
            key_node.as.string.length = current_token.text.length();
            uint32_t key_idx = add_node(key_node);
            advance();

            ASTNode access_node;
            access_node.type = NodeType::TableAccess;
            access_node.line = current_token.line;
            access_node.as.table_access.table = target;
            access_node.as.table_access.key = key_idx;
            target = add_node(access_node);
        }

        uint32_t func_expr = parse_funcbody(is_method);

        ctx.block_statements.push_back(target);
        ctx.block_statements.push_back(func_expr);

        ASTNode assign_node;
        assign_node.type = NodeType::Assignment;
        assign_node.line = line;
        assign_node.as.assign.first_target = ctx.block_statements.size() - 2;
        assign_node.as.assign.target_count = 1;
        assign_node.as.assign.first_value = ctx.block_statements.size() - 1;
        assign_node.as.assign.value_count = 1;
        return add_node(assign_node);
    }
    //------------------ PARSE: stmt - return statement with optional values
    if (current_token.type == TokenType::TokReturn) {
        int line = current_token.line;
        advance();
        std::vector<uint32_t> values;

        if (current_token.type != TokenType::TokEnd && current_token.type != TokenType::TokElseIf && current_token.type != TokenType::TokElse
            && current_token.type != TokenType::TokUntil && current_token.type != TokenType::TokEof && current_token.type != TokenType::TokSemicolon) {
            do {
                values.push_back(parse_expression());
            } while (current_token.type == TokenType::TokComma && (advance(), true));
        }

        uint32_t first_val = ctx.block_statements.size();
        for (auto v : values)
            ctx.block_statements.push_back(v);

        ASTNode node;
        node.type = NodeType::ReturnStatement;
        node.line = line;
        node.as.return_stmt.first_value = first_val;
        node.as.return_stmt.value_count = static_cast<uint32_t>(values.size());
        return add_node(node);
    }
    //------------------ PARSE: stmt - break statement (exits innermost loop)
    if (current_token.type == TokenType::TokBreak) {
        int line = current_token.line;
        advance();
        ASTNode node;
        node.type = NodeType::BreakStatement;
        node.line = line;
        return add_node(node);
    }
    //------------------ PARSE: stmt - do-end block statement
    if (current_token.type == TokenType::TokDo) {
        int line = current_token.line;
        advance();
        uint32_t body_block = parse_block(false);
        if (current_token.type == TokenType::TokEnd)
            advance();
        ASTNode node;
        node.type = NodeType::DoStatement;
        node.line = line;
        node.as.do_stmt.body_block = body_block;
        return add_node(node);
    }
    //------------------ PARSE: stmt - while loop
    if (current_token.type == TokenType::TokWhile) {
        int line = current_token.line;
        advance();
        uint32_t condition = parse_expression();
        if (current_token.type == TokenType::TokDo)
            advance();
        uint32_t body_block = parse_block(false);
        if (current_token.type == TokenType::TokEnd)
            advance();
        ASTNode node;
        node.type = NodeType::WhileStatement;
        node.line = line;
        node.as.while_stmt.condition = condition;
        node.as.while_stmt.body_block = body_block;
        return add_node(node);
    }
    //------------------ PARSE: stmt - repeat-until loop
    if (current_token.type == TokenType::TokRepeat) {
        int line = current_token.line;
        advance();
        uint32_t body_block = parse_block(false, true);
        uint32_t condition = INVALID_NODE;
        if (current_token.type == TokenType::TokUntil) {
            advance();
            condition = parse_expression();
        }
        leave_scope();
        ASTNode node;
        node.type = NodeType::RepeatStatement;
        node.line = line;
        node.as.repeat_stmt.body_block = body_block;
        node.as.repeat_stmt.condition = condition;
        return add_node(node);
    }
    //------------------ PARSE: stmt - numeric or generic for loop
    if (current_token.type == TokenType::TokFor) {
        int line = current_token.line;
        advance();

        std::vector<uint32_t> vars;
        if (current_token.type != TokenType::TokIdent)
            throw std::runtime_error(
                "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": Expected identifier");

        do {
            ASTNode ident_node;
            ident_node.type = NodeType::Identifier;
            ident_node.line = current_token.line;
            ident_node.as.ident.name = current_token.text.data();
            ident_node.as.ident.length = current_token.text.length();
            ident_node.as.ident.is_captured = false;
            ident_node.as.ident.is_global = false;
            ident_node.as.ident.attr = Attribute::None;
            uint32_t var_id = add_node(ident_node);
            vars.push_back(var_id);
            add_symbol(current_token.text, SymbolType::Local, var_id);
            advance();
        } while (current_token.type == TokenType::TokComma && (advance(), true));

        if (current_token.type == TokenType::TokAssign) {
            if (vars.size() > 1)
                throw std::runtime_error("Error: numeric for only accepts one variable");
            advance();
            uint32_t start_expr = parse_expression();
            if (current_token.type != TokenType::TokComma)
                throw std::runtime_error("Error: expected ',' in for loop");
            advance();
            uint32_t limit_expr = parse_expression();
            uint32_t step_expr = 0xFFFFFFFF;
            if (current_token.type == TokenType::TokComma) {
                advance();
                step_expr = parse_expression();
            }
            if (current_token.type == TokenType::TokDo)
                advance();
            uint32_t body_block = parse_block(false);
            if (current_token.type == TokenType::TokEnd)
                advance();

            ASTNode node;
            node.type = NodeType::ForStatement;
            node.line = line;
            node.as.for_stmt.var_ident = vars[0];
            node.as.for_stmt.start_expr = start_expr;
            node.as.for_stmt.limit_expr = limit_expr;
            node.as.for_stmt.step_expr = step_expr;
            node.as.for_stmt.body_block = body_block;
            return add_node(node);
        } else if (current_token.type == TokenType::TokIdent && current_token.text == "in") {
            advance();
            std::vector<uint32_t> iter_exprs;
            do {
                iter_exprs.push_back(parse_expression());
            } while (current_token.type == TokenType::TokComma && (advance(), true));
            if (current_token.type == TokenType::TokDo)
                advance();
            uint32_t body_block = parse_block(false);
            if (current_token.type == TokenType::TokEnd)
                advance();

            uint32_t first_var = ctx.block_statements.size();
            for (auto v : vars)
                ctx.block_statements.push_back(v);
            uint32_t first_iter = ctx.block_statements.size();
            for (auto e : iter_exprs)
                ctx.block_statements.push_back(e);

            ASTNode node;
            node.type = NodeType::GenericForStatement;
            node.line = line;
            node.as.generic_for.first_var = first_var;
            node.as.generic_for.var_count = vars.size();
            node.as.generic_for.first_iter = first_iter;
            node.as.generic_for.iter_count = iter_exprs.size();
            node.as.generic_for.body_block = body_block;
            return add_node(node);
        } else {
            throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                + ": Expected '=' or 'in' for loop");
        }
    }
    //------------------ PARSE: stmt - if-then-elseif-else conditional statement
    if (current_token.type == TokenType::TokIf) {
        int line = current_token.line;
        advance();
        uint32_t condition = parse_expression();
        if (current_token.type == TokenType::TokThen)
            advance();
        uint32_t then_block = parse_block(false);
        uint32_t else_block = INVALID_NODE;
        if (current_token.type == TokenType::TokElseIf) {
            current_token.type = TokenType::TokIf;
            else_block = parse_statement();
        } else if (current_token.type == TokenType::TokElse) {
            advance();
            else_block = parse_block(false);
            if (current_token.type == TokenType::TokEnd)
                advance();
        } else if (current_token.type == TokenType::TokEnd) {
            advance();
        }
        ASTNode node;
        node.type = NodeType::IfStatement;
        node.line = line;
        node.as.if_stmt.condition = condition;
        node.as.if_stmt.then_block = then_block;
        node.as.if_stmt.else_block = else_block;
        return add_node(node);
    }
    //------------------ PARSE: stmt - local variable declaration (with optional attribute/function)
    if (current_token.type == TokenType::TokLocal) {
        int line = current_token.line;
        advance();

        Attribute decl_attr = Attribute::None;
        SymbolType decl_sym_type = SymbolType::Local;
        if (current_token.type == TokenType::TokLess) {
            advance();
            if (current_token.type == TokenType::TokIdent && current_token.text == "const") {
                decl_attr = Attribute::Const;
                decl_sym_type = SymbolType::Const;
            } else if (current_token.type == TokenType::TokIdent && current_token.text == "close") {
                decl_attr = Attribute::Close;
                decl_sym_type = SymbolType::Close;
            } else {
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": unknown attribute");
            }
            advance();
            if (current_token.type != TokenType::TokGreater) {
                throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                    + ": expected '>' after attribute");
            }
            advance();
        }

        if (current_token.type == TokenType::TokFunction) {
            advance();
            if (current_token.type != TokenType::TokIdent) {
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": Expected function name");
            }

            ASTNode ident_node;
            ident_node.type = NodeType::Identifier;
            ident_node.line = current_token.line;
            ident_node.as.ident.name = current_token.text.data();
            ident_node.as.ident.length = current_token.text.length();
            ident_node.as.ident.is_captured = false;
            ident_node.as.ident.is_global = false;
            ident_node.as.ident.attr = Attribute::None;
            uint32_t target = add_node(ident_node);

            add_symbol(current_token.text, SymbolType::Local, target);
            advance();

            uint32_t func_expr = parse_funcbody(false);

            ctx.block_statements.push_back(target);
            ctx.block_statements.push_back(func_expr);

            ASTNode node;
            node.type = NodeType::LocalDecl;
            node.line = line;
            node.as.local_decl.first_ident = ctx.block_statements.size() - 2;
            node.as.local_decl.ident_count = 1;
            node.as.local_decl.first_value = ctx.block_statements.size() - 1;
            node.as.local_decl.value_count = 1;
            return add_node(node);
        }

        std::vector<uint32_t> idents;
        do {
            if (current_token.type != TokenType::TokIdent) {
                throw std::runtime_error(
                    "Error: " + ctx.filename + ":" + std::to_string(current_token.line) + ": Identifier expected");
            }
            ASTNode ident_node;
            ident_node.type = NodeType::Identifier;
            ident_node.line = current_token.line;
            ident_node.as.ident.name = current_token.text.data();
            ident_node.as.ident.length = current_token.text.length();
            ident_node.as.ident.is_captured = false;
            ident_node.as.ident.is_global = false;

            std::string_view name = current_token.text;
            advance();

            Attribute attr = decl_attr;
            SymbolType sym_type = decl_sym_type;

            if (current_token.type == TokenType::TokLess) {
                advance();
                if (current_token.type == TokenType::TokIdent && current_token.text == "const") {
                    attr = Attribute::Const;
                    sym_type = SymbolType::Const;
                } else if (current_token.type == TokenType::TokIdent && current_token.text == "close") {
                    attr = Attribute::Close;
                    sym_type = SymbolType::Close;
                } else {
                    throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                        + ": unknown attribute '" + std::string(current_token.text) + "'");
                }
                advance();

                if (current_token.type != TokenType::TokGreater) {
                    throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
                        + ": expected '>' after attribute");
                }
                advance();
            }

            ident_node.as.ident.attr = attr;
            uint32_t id_idx = add_node(ident_node);
            idents.push_back(id_idx);
            add_symbol(name, sym_type, id_idx);

        } while (current_token.type == TokenType::TokComma && (advance(), true));

        std::vector<uint32_t> values;
        if (current_token.type == TokenType::TokAssign) {
            advance();
            do {
                values.push_back(parse_expression());
            } while (current_token.type == TokenType::TokComma && (advance(), true));
        }

        uint32_t first_id = ctx.block_statements.size();
        for (auto id : idents)
            ctx.block_statements.push_back(id);
        uint32_t first_val = ctx.block_statements.size();
        for (auto v : values)
            ctx.block_statements.push_back(v);

        ASTNode node;
        node.type = NodeType::LocalDecl;
        node.line = line;
        node.as.local_decl.first_ident = first_id;
        node.as.local_decl.ident_count = static_cast<uint32_t>(idents.size());
        node.as.local_decl.first_value = first_val;
        node.as.local_decl.value_count = static_cast<uint32_t>(values.size());
        return add_node(node);
    }
    //------------------ PARSE: stmt - assignment expression or function call
    if (current_token.type == TokenType::TokIdent || current_token.type == TokenType::TokLParen) {
        int line = current_token.line;

        std::vector<uint32_t> targets;
        do {
            targets.push_back(parse_postfix_expression());
        } while (current_token.type == TokenType::TokComma && (advance(), true));

        if (current_token.type == TokenType::TokAssign) {
            advance();
            std::vector<uint32_t> values;
            do {
                values.push_back(parse_expression());
            } while (current_token.type == TokenType::TokComma && (advance(), true));

            for (uint32_t t : targets) {
                if (ctx.nodes[t].type == NodeType::Identifier) {
                    std::string_view name(ctx.nodes[t].as.ident.name, ctx.nodes[t].as.ident.length);
                    bool dummy_cap, dummy_glob;
                    SymbolType sym = resolve_symbol(name, line, dummy_cap, dummy_glob);
                    if (sym == SymbolType::Const || sym == SymbolType::ConstGlobal) {
                        throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(line)
                            + ": assignment to const variable '" + std::string(name) + "'");
                    }
                }
            }

            uint32_t first_target = ctx.block_statements.size();
            for (auto t : targets)
                ctx.block_statements.push_back(t);
            uint32_t first_val = ctx.block_statements.size();
            for (auto v : values)
                ctx.block_statements.push_back(v);

            ASTNode node;
            node.type = NodeType::Assignment;
            node.line = line;
            node.as.assign.first_target = first_target;
            node.as.assign.target_count = static_cast<uint32_t>(targets.size());
            node.as.assign.first_value = first_val;
            node.as.assign.value_count = static_cast<uint32_t>(values.size());
            return add_node(node);
        }
        if (targets.size() > 1) {
            throw std::runtime_error(
                "Error: " + ctx.filename + ":" + std::to_string(line) + ": syntax error, unexpected ','");
        }
        return targets[0];
    }

    uint32_t res = parse_expression();
    if (res == INVALID_NODE && current_token.type != TokenType::TokEof) {
        throw std::runtime_error("Error: " + ctx.filename + ":" + std::to_string(current_token.line)
            + ": unexpected token '" + std::string(current_token.text) + "'");
    }
    return res;
}

//------------------ PARSER: parse_block - parses a sequence of statements, manages scope entry/exit
uint32_t Parser::parse_block(bool is_main, bool keep_scope_open) {
    int line = current_token.line;
    std::vector<uint32_t> statements;

    if (!is_main)
        enter_scope();

    while (current_token.type != TokenType::TokEof) {
        if (!is_main
            && (current_token.type == TokenType::TokElseIf || current_token.type == TokenType::TokElse || current_token.type == TokenType::TokEnd
                || current_token.type == TokenType::TokUntil)) {
            break;
        }
        uint32_t stmt = parse_statement();
        if (stmt != INVALID_NODE)
            statements.push_back(stmt);
    }

    if (!is_main && !keep_scope_open)
        leave_scope();

    ASTNode node;
    node.type = NodeType::Block;
    node.line = line;
    node.as.block.count = statements.size();

    uint32_t first_idx = ctx.block_statements.size();
    for (uint32_t stmt : statements) {
        ctx.block_statements.push_back(stmt);
    }
    node.as.block.first_statement = first_idx;

    return add_node(node);
}

//------------------ PARSER: parse_funcbody - parses function parameters and body block
uint32_t Parser::parse_funcbody(bool is_method) {
    int line = current_token.line;
    if (current_token.type != TokenType::TokLParen)
        throw std::runtime_error("Expected '(' for function parameters");
    advance();

    current_function_depth++;
    enter_scope();

    std::vector<uint32_t> params;
    bool is_vararg = false;
    uint32_t named_vararg = 0xFFFFFFFF;

    if (is_method) {
        ASTNode self_node;
        self_node.type = NodeType::Identifier;
        self_node.line = line;
        self_node.as.ident.name = "self";
        self_node.as.ident.length = 4;
        self_node.as.ident.is_captured = false;
        self_node.as.ident.is_global = false;
        self_node.as.ident.attr = Attribute::None;
        uint32_t p_idx = add_node(self_node);
        params.push_back(p_idx);
        add_symbol("self", SymbolType::Local, p_idx);
    }

    if (current_token.type != TokenType::TokRParen) {
        do {
            if (current_token.type == TokenType::TokVararg) {
                is_vararg = true;
                advance();
                if (current_token.type == TokenType::TokIdent) {
                    ASTNode va_node;
                    va_node.type = NodeType::Identifier;
                    va_node.line = current_token.line;
                    va_node.as.ident.name = current_token.text.data();
                    va_node.as.ident.length = current_token.text.length();
                    va_node.as.ident.is_captured = false;
                    va_node.as.ident.is_global = false;
                    va_node.as.ident.attr = Attribute::None;
                    named_vararg = add_node(va_node);
                    add_symbol(current_token.text, SymbolType::Local, named_vararg);
                    advance();
                }
                break;
            } else if (current_token.type == TokenType::TokIdent) {
                ASTNode param_node;
                param_node.type = NodeType::Identifier;
                param_node.line = current_token.line;
                param_node.as.ident.name = current_token.text.data();
                param_node.as.ident.length = current_token.text.length();
                param_node.as.ident.is_captured = false;
                param_node.as.ident.is_global = false;
                param_node.as.ident.attr = Attribute::None;
                uint32_t p_idx = add_node(param_node);
                params.push_back(p_idx);
                add_symbol(current_token.text, SymbolType::Local, p_idx);
                advance();
            } else {
                throw std::runtime_error("Expected parameter name or '...'");
            }
        } while (current_token.type == TokenType::TokComma && (advance(), true));
    }

    if (current_token.type != TokenType::TokRParen)
        throw std::runtime_error("Expected ')' after parameters");
    advance();

    std::vector<uint32_t> statements;
    while (current_token.type != TokenType::TokEof && current_token.type != TokenType::TokEnd) {
        uint32_t stmt = parse_statement();
        if (stmt != INVALID_NODE)
            statements.push_back(stmt);
    }

    leave_scope();
    current_function_depth--;

    if (current_token.type != TokenType::TokEnd)
        throw std::runtime_error("Expected 'end' to close function");
    advance();

    ASTNode body_node;
    body_node.type = NodeType::Block;
    body_node.line = line;
    body_node.as.block.count = statements.size();
    uint32_t first_stmt_idx = ctx.block_statements.size();
    for (uint32_t stmt : statements)
        ctx.block_statements.push_back(stmt);
    body_node.as.block.first_statement = first_stmt_idx;
    uint32_t body = add_node(body_node);

    uint32_t first_param = ctx.block_statements.size();
    for (auto p : params)
        ctx.block_statements.push_back(p);

    ASTNode fnode;
    fnode.type = NodeType::FunctionDef;
    fnode.line = line;
    fnode.as.func_def.first_param = first_param;
    fnode.as.func_def.param_count = static_cast<uint32_t>(params.size());
    fnode.as.func_def.is_vararg = is_vararg;
    fnode.as.func_def.named_vararg_ident = named_vararg;
    fnode.as.func_def.body_block = body;
    return add_node(fnode);
}

//------------------ PARSER: parse - entry point, parses the entire source as a main block
uint32_t Parser::parse() {
    return parse_block(true);
}

}