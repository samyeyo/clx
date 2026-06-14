// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  visitor.h · Visitor Header                 │
// └─────────────────────────────────────────────┘

#ifndef SYNTAX_VISITOR_H
#define SYNTAX_VISITOR_H
#include "nodes.h"
namespace clx {
//------------------ Visitor: base class for AST traversal with per-node-type callbacks
class Visitor {
public:
    virtual ~Visitor() = default;
    //------------------ visit: generic entry point dispatched by node type
    virtual void visit(const ASTNode& node) = 0;
    //------------------ visitBlock: called when visiting a Block node
    virtual void visitBlock(const ASTNode& node) { (void)node; }
    virtual void visitLocalDecl(const ASTNode& node) { (void)node; }
    virtual void visitGlobalDecl(const ASTNode& node) { (void)node; }
    virtual void visitAssignment(const ASTNode& node) { (void)node; }
    virtual void visitBinaryOp(const ASTNode& node) { (void)node; }
    virtual void visitNumber(const ASTNode& node) { (void)node; }
    virtual void visitInteger(const ASTNode& node) { (void)node; }
    virtual void visitIdentifier(const ASTNode& node) { (void)node; }
    virtual void visitString(const ASTNode& node) { (void)node; }
    virtual void visitDoStatement(const ASTNode& node) { (void)node; }
    virtual void visitCallExpression(const ASTNode& node) { (void)node; }
    virtual void visitIfStatement(const ASTNode& node) { (void)node; }
    virtual void visitWhileStatement(const ASTNode& node) { (void)node; }
    virtual void visitRepeatStatement(const ASTNode& node) { (void)node; }
    virtual void visitForStatement(const ASTNode& node) { (void)node; }
    virtual void visitGenericForStatement(const ASTNode& node) { (void)node; }
    virtual void visitTableConstructor(const ASTNode& node) { (void)node; }
    virtual void visitTableAccess(const ASTNode& node) { (void)node; }
    virtual void visitFunctionDef(const ASTNode& node) { (void)node; }
    virtual void visitReturnStatement(const ASTNode& node) { (void)node; }
    virtual void visitVararg(const ASTNode& node) { (void)node; }
    virtual void visitTrueLiteral(const ASTNode& node) { (void)node; }
    virtual void visitFalseLiteral(const ASTNode& node) { (void)node; }
    virtual void visitNilLiteral(const ASTNode& node) { (void)node; }
    virtual void visitUnaryOp(const ASTNode& node) { (void)node; }
    virtual void visitIntrinsicCall(const ASTNode& node) { (void)node; }
    virtual void visitParenExpression(const ASTNode& node) { (void)node; }
    virtual void visitLabelStatement(const ASTNode& node) { (void)node; }
    virtual void visitGotoStatement(const ASTNode& node) { (void)node; }
};
//------------------ traverse: walk the AST starting from a given node index
void traverse(const ASTContext& ctx, uint32_t node_idx, Visitor& visitor);
//------------------ traverseNode: visit a single AST node and recurse into children
void traverseNode(const ASTContext& ctx, const ASTNode& node, Visitor& visitor);
}
#endif