// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  lexer.h · Lexer Header                     │
// └─────────────────────────────────────────────┘

#ifndef SYNTAX_LEXER_H
#define SYNTAX_LEXER_H
#include "nodes.h"
#include <string_view>
namespace clx {

//------------------ Lexer: tokenizes Lua source text into a stream of tokens
class Lexer {
public:
    //------------------ Lexer: constructs lexer from source string and filename
    explicit Lexer(std::string_view source, const char* filename);
    //------------------ advance: moves to the next token in the source
    void advance();
    //------------------ current: returns the current token without advancing
    const Token& current() const;
    //------------------ remaining_source: returns the unconsumed portion of source
    std::string_view remaining_source() const;
    //------------------ position: returns the current byte offset in source
    size_t position() const;
    //------------------ line: returns the current line number
    int line() const;
    //------------------ filename: returns the source filename
    const char* filename() const;
private:
    //------------------ src: the full source text being lexed
    std::string_view src;
    //------------------ pos: current byte offset in src
    size_t pos;
    //------------------ current_token: the most recently lexed token
    Token current_token;
    //------------------ current_line: current line number in source
    int current_line;
    //------------------ filename_str: owning storage for the source filename
    std::string filename_str;
};
}
#endif