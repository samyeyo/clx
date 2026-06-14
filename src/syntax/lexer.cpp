// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  lexer.cpp · Lua Lexer                      │
// └─────────────────────────────────────────────┘

#include "lexer.h"
#include <cctype>
#include <stdexcept>
namespace clx {
using enum TokenType;


//------------------ LEXER: constructor - initializes lexer with source text and filename
Lexer::Lexer(std::string_view source, const char* filename)
    : src(source), pos(0), current_line(1), filename_str(filename) {
    advance();
}

//------------------ LEXER: remaining_source - returns unprocessed source text after current position
std::string_view Lexer::remaining_source() const {
    return src.substr(pos);
}


//------------------ LEXER: advance - scans next token, skipping whitespace and comments
void Lexer::advance() {
    while (pos < src.length()) {
        if (src[pos] == '\n') {
            current_line++; pos++; continue;
        }
        if (std::isspace(src[pos])) {
            pos++; continue;
        }
        if (src[pos] == '-' && pos + 1 < src.length() && src[pos+1] == '-') {
            pos += 2;
            if (pos < src.length() && src[pos] == '[') {
                size_t eq = 0;
                while (pos + eq + 1 < src.length() && src[pos + 1 + eq] == '=') eq++;
                if (pos + 1 + eq < src.length() && src[pos + 1 + eq] == '[') {
                    pos += 2 + eq;
                    std::string close;
                    close += ']';
                    close.append(eq, '=');
                    close += ']';
                    while (pos + eq + 1 < src.length()) {
                        if (src[pos] == '\n') current_line++;
                        if (src.compare(pos, close.size(), close) == 0) {
                            pos += close.size();
                            break;
                        }
                        pos++;
                    }
                } else {
                    while (pos < src.length() && src[pos] != '\n') pos++;
                }
            } else {
                while (pos < src.length() && src[pos] != '\n') pos++;
            }
            continue;
        }
        if (src[pos] == '#' && pos + 1 < src.length() && src[pos + 1] == '!') {
            while (pos < src.length() && src[pos] != '\n') pos++;
            continue;
        }
        break;
    }
    if (pos >= src.length()) {
        current_token = Token{TokEof, std::string_view(), 0.0, current_line};
        return;
    }
    if (std::isalpha(src[pos]) || src[pos] == '_') {
        size_t start = pos;
        while (pos < src.length() && (std::isalnum(src[pos]) || src[pos] == '_')) pos++;
        std::string_view text(src.data() + start, pos - start);
        TokenType type = TokIdent;
        if (text == "local") type = TokLocal;
        else if (text == "global") type = TokGlobal;
        else if (text == "if") type = TokIf;
        else if (text == "then") type = TokThen;
        else if (text == "elseif") type = TokElseIf;
        else if (text == "else") type = TokElse;
        else if (text == "end") type = TokEnd;
        else if (text == "while") type = TokWhile;
        else if (text == "do") type = TokDo;
        else if (text == "repeat") type = TokRepeat;
        else if (text == "until") type = TokUntil;
        else if (text == "for") type = TokFor;
        else if (text == "function") type = TokFunction;
        else if (text == "return") type = TokReturn;
        else if (text == "and") type = TokAnd;
        else if (text == "or") type = TokOr;
        else if (text == "not") type = TokNot;
        else if (text == "true") type = TokTrue;
        else if (text == "false") type = TokFalse;
        else if (text == "nil") type = TokNil;
        else if (text == "goto") type = TokGoto;
        else if (text == "break") type = TokBreak;
        current_token = {type, text, 0, current_line};
        return;
    }
    if (std::isdigit(src[pos])) {
        char* end;
        double val = std::strtod(src.data() + pos, &end);
        size_t len = end - (src.data() + pos);
        std::string_view text(src.data() + pos, len);
        pos += len;
        current_token = {TokNumber, text, val, current_line};
        return;
    }
    if (src[pos] == '"' || src[pos] == '\'') {
        char quote = src[pos++];
        size_t start = pos;
        while (pos < src.length() && src[pos] != quote) {
            if (src[pos] == '\n') current_line++;
            if (src[pos] == '\\' && pos + 1 < src.length()) pos++;
            pos++;
        }
        std::string_view text(src.data() + start, pos - start);
        if (pos < src.length() && src[pos] == quote) pos++;
        current_token = {TokString, text, 0, current_line};
        return;
    }
    char c = src[pos++];
    switch (c) {
        case '+': current_token = {TokPlus, "+", 0, current_line}; return;
        case '-': current_token = {TokMinus, "-", 0, current_line}; return;
        case '*': current_token = {TokStar, "*", 0, current_line}; return;
        case '/':
            if (pos < src.length() && src[pos] == '/') { pos++; current_token = {TokFloorDiv, "//", 0, current_line}; }
            else { current_token = {TokSlash, "/", 0, current_line}; }
            return;
        case '(': current_token = {TokLParen, "(", 0, current_line}; return;
        case ')': current_token = {TokRParen, ")", 0, current_line}; return;
        case '{': current_token = {TokLBrace, "{", 0, current_line}; return;
        case '}': current_token = {TokRBrace, "}", 0, current_line}; return;
        case '[':
            {
                size_t eq = 0;
                while (pos + eq < src.length() && src[pos + eq] == '=') eq++;
                if (pos + eq < src.length() && src[pos + eq] == '[') {
                    size_t close_eq = eq;
                    pos += eq + 1;
                    size_t start = pos;
                    std::string close;
                    close += ']';
                    close.append(close_eq, '=');
                    close += ']';
                    while (pos + close.size() <= src.length()) {
                        if (src[pos] == '\n') current_line++;
                        if (src.compare(pos, close.size(), close) == 0) {
                            std::string_view text(src.data() + start, pos - start);
                            pos += close.size();
                            current_token = {TokString, text, 0, current_line};
                            return;
                        }
                        pos++;
                    }
                    throw std::runtime_error(std::to_string(current_line) + ": unfinished long string");
                }
                current_token = {TokLBracket, "[", 0, current_line};
            }
            return;
        case ']': current_token = {TokRBracket, "]", 0, current_line}; return;
        case ':':
            if (pos < src.length() && src[pos] == ':') { pos++; current_token = {TokDoubleColon, "::", 0, current_line}; }
            else { current_token = {TokColon, ":", 0, current_line}; }
            return;
        case '#': current_token = {TokLen, "#", 0, current_line}; return;
        case '.':
            if (pos + 1 < src.length() && src[pos] == '.' && src[pos+1] == '.') { pos += 2; current_token = {TokVararg, "...", 0, current_line}; }
            else if (pos < src.length() && src[pos] == '.') { pos++; current_token = {TokConcat, "..", 0, current_line}; }
            else if (pos < src.length() && std::isdigit(src[pos])) {
                char* end = nullptr;
                double val = std::strtod(src.data() + pos - 1, &end);
                size_t len = end - (src.data() + pos - 1);
                current_token = {TokNumber, std::string_view(src.data() + pos - 1, len), val, current_line};
                pos = end - src.data();
            } else { current_token = {TokDot, ".", 0, current_line}; }
            return;
        case ',': current_token = {TokComma, ",", 0, current_line}; return;
        case ';': current_token = {TokSemicolon, ";", 0, current_line}; return;
        case '=':
            if (pos < src.length() && src[pos] == '=') { pos++; current_token = {TokEqEq, "==", 0, current_line}; }
            else { current_token = {TokAssign, "=", 0, current_line}; }
            return;
        case '<':
            if (pos < src.length() && src[pos] == '<') { pos++; current_token = {TokShl, "<<", 0, current_line}; }
            else if (pos < src.length() && src[pos] == '=') { pos++; current_token = {TokLessEq, "<=", 0, current_line}; }
            else { current_token = {TokLess, "<", 0, current_line}; }
            return;
        case '>':
            if (pos < src.length() && src[pos] == '>') { pos++; current_token = {TokShr, ">>", 0, current_line}; }
            else if (pos < src.length() && src[pos] == '=') { pos++; current_token = {TokGreaterEq, ">=", 0, current_line}; }
            else { current_token = {TokGreater, ">", 0, current_line}; }
            return;
        case '~':
            if (pos < src.length() && src[pos] == '=') { pos++; current_token = {TokNotEq, "~=", 0, current_line}; }
            else current_token = {TokBitXor, "~", 0, current_line};
            return;
        case '%': current_token = {TokMod, "%", 0, current_line}; return;
        case '^': current_token = {TokPow, "^", 0, current_line}; return;
        case '&': current_token = {TokBitAnd, "&", 0, current_line}; return;
        case '|': current_token = {TokBitOr, "|", 0, current_line}; return;
    }
    current_token = {TokEof, "", 0, current_line};
}

//------------------ LEXER: current - returns current token
const Token& Lexer::current() const { return current_token; }
//------------------ LEXER: position - returns current source byte position
size_t Lexer::position() const { return pos; }
//------------------ LEXER: line - returns current line number
int Lexer::line() const { return current_line; }
//------------------ LEXER: filename - returns source filename
const char* Lexer::filename() const { return filename_str.c_str(); }
}
