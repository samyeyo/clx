// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  nodes.h · AST Node Definitions             │
// └─────────────────────────────────────────────┘

#ifndef SYNTAX_NODES_H
#define SYNTAX_NODES_H
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
namespace clx {


//------------------ NodeType: enumerates all AST node kinds
enum class NodeType {
    Block, LocalDecl, GlobalDeclStatement, Assignment, BinaryOp,
    Number, Integer, Identifier, String, DoStatement, CallExpression,
    IfStatement, WhileStatement, RepeatStatement, ForStatement, GenericForStatement,
    TableConstructor, TableAccess,
    FunctionDef, ReturnStatement, Vararg, TrueLiteral, FalseLiteral, NilLiteral,
    UnaryOp, IntrinsicCall, ParenExpression,
    LabelStatement, GotoStatement, BreakStatement
};


//------------------ SymbolType: classification of variable symbols (local, const, close, etc)
enum class SymbolType { Local, Const, Close, ExplicitGlobal, ConstGlobal };


//------------------ Symbol: represents a variable in the symbol table with scope info
struct Symbol {
    std::string_view name;
    SymbolType type;
    int depth;
    int function_depth;
    uint32_t decl_idx;
};

//------------------ Attribute: variable attribute qualifiers
enum class Attribute { None, Const, Close };
//------------------ ImplicitGlobalMode: controls implicit global variable access
enum class ImplicitGlobalMode { None, ReadWrite, ReadOnly };


//------------------ BinaryOp: binary operator codes mapped to Lua opcodes
enum class BinaryOp : int { Add=1, Sub=2, Mul=3, Div=4, Eq=5, Lt=6, Gt=7, Le=8, Ge=9, Ne=10, And=11, Or=12, Concat=13, Mod=14, FloorDiv=15, Pow=16, BitAnd=17, BitOr=18, BitXor=19, Shl=20, Shr=21 };

//------------------ UnaryOp: unary operator codes
enum class UnaryOp : int { Len=1, Minus=2, BNot=3, Not=4 };

//------------------ Intrinsic: builtin math/string function identifiers for fast dispatch
enum class Intrinsic : int { Sin, Cos, Floor, Ceil, Abs, Sqrt, FMod, TypeFn, ToStringFn, Log, Exp, Tan, ATan, ASin, ACos, SinH, CosH, TanH, ATan2, PowFn, Deg, Rad };


//------------------ ASTNode: tagged union node in the arena-allocated AST
struct ASTNode {
    //------------------ type: discriminator for the active union variant
    NodeType type;
    //------------------ line: source line number for error reporting
    int line;
    union {
        //------------------ block: statement block with first statement index and count
        struct { uint32_t first_statement; uint32_t count; } block;
        //------------------ global_decl: global variable declaration
        struct { uint32_t first_ident; uint32_t ident_count; uint32_t first_value; uint32_t value_count; bool is_wildcard; Attribute attr; } global_decl;
        //------------------ local_decl: local variable declaration
        struct { uint32_t first_ident; uint32_t ident_count; uint32_t first_value; uint32_t value_count; } local_decl;
        //------------------ assign: multi-target assignment
        struct { uint32_t first_target; uint32_t target_count; uint32_t first_value; uint32_t value_count; } assign;
        //------------------ bin_op: binary operation with left/right node indices
        struct { uint32_t left; uint32_t right; int op; } bin_op;
        //------------------ number: floating point literal value
        struct { double val; } number;
        //------------------ integer: integer literal value
        struct { int64_t val; } integer;
        //------------------ ident: identifier with capture/global/attribute info
        struct { const char* name; size_t length; bool is_captured; bool is_global; Attribute attr; } ident;
        //------------------ string: string literal content
        struct { const char* text; size_t length; } string;
        //------------------ call_expr: function call expression
        struct { uint32_t target; uint32_t first_arg; uint32_t arg_count; } call_expr;
        //------------------ if_stmt: conditional branch with then/else blocks
        struct { uint32_t condition; uint32_t then_block; uint32_t else_block; } if_stmt;
        //------------------ while_stmt: while loop
        struct { uint32_t condition; uint32_t body_block; } while_stmt;
        //------------------ repeat_stmt: repeat-until loop
        struct { uint32_t body_block; uint32_t condition; } repeat_stmt;
        //------------------ for_stmt: numeric for loop
        struct { uint32_t var_ident; uint32_t start_expr; uint32_t limit_expr; uint32_t step_expr; uint32_t body_block; } for_stmt;
        //------------------ table_cons: table constructor with items
        struct { uint32_t first_item; uint32_t count; } table_cons;
        //------------------ generic_for: generic for loop (iterators)
        struct { uint32_t first_var; uint32_t var_count; uint32_t first_iter; uint32_t iter_count; uint32_t body_block; } generic_for;
        //------------------ table_access: table index access (table[key])
        struct { uint32_t table; uint32_t key; } table_access;
        //------------------ do_stmt: do-end block
        struct { uint32_t body_block; } do_stmt;
        //------------------ func_def: function definition with parameters and body
        struct { uint32_t first_param; uint32_t param_count; bool is_vararg; uint32_t named_vararg_ident; uint32_t body_block; } func_def;
        //------------------ return_stmt: return statement
        struct { uint32_t first_value; uint32_t value_count; } return_stmt;
        //------------------ unary_op: unary operation
        struct { uint32_t expr; int op; } unary_op;
        //------------------ intrinsic_call: call to an intrinsic builtin function
        struct { int func; uint32_t first_arg; uint32_t arg_count; } intrinsic_call;
        //------------------ paren_expr: parenthesized expression
        struct { uint32_t expr; } paren_expr;
        //------------------ label_stmt: label for goto
        struct { uint32_t name_ident; } label_stmt;
        //------------------ goto_stmt: goto statement to a label
        struct { uint32_t name_ident; } goto_stmt;
        //------------------ break_stmt: break statement with no extra data
        struct { } break_stmt;
    } as;
};


//------------------ ASTContext: holds the arena and metadata for a parsed file
struct ASTContext {
    std::string filename;
    std::vector<ASTNode> nodes;
    std::vector<uint32_t> block_statements;
};


//------------------ TokenType: all token kinds produced by the lexer
enum class TokenType {
    TokEof = 0, TokIdent, TokNumber, TokString, TokLocal, TokGlobal,
    TokAssign, TokEqEq, TokPlus, TokMinus, TokStar, TokSlash,
    TokLParen, TokRParen, TokComma, TokLess, TokGreater,
    TokLBrace, TokRBrace, TokLBracket, TokRBracket, TokDot,
    TokLessEq, TokGreaterEq, TokNotEq,
    TokIf, TokThen, TokElseIf, TokElse, TokEnd,
    TokWhile, TokDo, TokRepeat, TokUntil, TokFor,
    TokFunction, TokReturn, TokColon, TokVararg, TokAnd, TokOr, TokNot,
    TokTrue, TokFalse, TokNil, TokLen, TokConcat,
    TokMod, TokFloorDiv, TokPow,
    TokBitAnd, TokBitOr, TokBitXor, TokShl, TokShr,
    TokSemicolon, TokGoto, TokDoubleColon, TokBreak
};


//------------------ Token: a single lexed token with its type, text, and value
struct Token {
    TokenType type = TokenType::TokEof;
    std::string_view text;
    double number_value = 0.0;
    int line = 0;
};

}
#endif