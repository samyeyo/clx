// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  visitor.cpp · AST Visitor Traversal        │
// └─────────────────────────────────────────────┘

#include "visitor.h"

namespace clx {



//------------------ VISITOR: traverse - starts AST traversal from a given node index, validates bounds
void traverse(const ASTContext& ctx, uint32_t node_idx, Visitor& visitor) {
    if (node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size()) return;
    const auto& n = ctx.nodes[node_idx];
    traverseNode(ctx, n, visitor);
}


//------------------ VISITOR: traverseNode - visits a node and recurses into all child nodes according to type
void traverseNode(const ASTContext& ctx, const ASTNode& node, Visitor& visitor) {
    visitor.visit(node);

    switch (node.type) {
        case NodeType::Block: {
            for (uint32_t i = 0; i < node.as.block.count; ++i) {
                uint32_t si = ctx.block_statements[node.as.block.first_statement + i];
                if (si < ctx.nodes.size()) traverse(ctx, si, visitor);
            }
            break;
        }
        case NodeType::LocalDecl:
        case NodeType::GlobalDeclStatement:
        case NodeType::Assignment: {
            uint32_t v_count = (node.type == NodeType::LocalDecl) ? node.as.local_decl.value_count
                               : (node.type == NodeType::GlobalDeclStatement) ? node.as.global_decl.value_count
                               : node.as.assign.value_count;
            uint32_t f_value = (node.type == NodeType::LocalDecl) ? node.as.local_decl.first_value
                               : (node.type == NodeType::GlobalDeclStatement) ? node.as.global_decl.first_value
                               : node.as.assign.first_value;
            for (uint32_t i = 0; i < v_count; ++i)
                traverse(ctx, ctx.block_statements[f_value + i], visitor);
            break;
        }
        case NodeType::BinaryOp:
            traverse(ctx, node.as.bin_op.left, visitor);
            traverse(ctx, node.as.bin_op.right, visitor);
            break;
        case NodeType::UnaryOp:
            traverse(ctx, node.as.unary_op.expr, visitor);
            break;
        case NodeType::CallExpression: {
            traverse(ctx, node.as.call_expr.target, visitor);
            for (uint32_t i = 0; i < node.as.call_expr.arg_count; ++i)
                traverse(ctx, ctx.block_statements[node.as.call_expr.first_arg + i], visitor);
            break;
        }
        case NodeType::IfStatement:
            traverse(ctx, node.as.if_stmt.condition, visitor);
            traverse(ctx, node.as.if_stmt.then_block, visitor);
            if (node.as.if_stmt.else_block != 0xFFFFFFFF)
                traverse(ctx, node.as.if_stmt.else_block, visitor);
            break;
        case NodeType::WhileStatement:
            traverse(ctx, node.as.while_stmt.condition, visitor);
            traverse(ctx, node.as.while_stmt.body_block, visitor);
            break;
        case NodeType::RepeatStatement:
            traverse(ctx, node.as.repeat_stmt.body_block, visitor);
            traverse(ctx, node.as.repeat_stmt.condition, visitor);
            break;
        case NodeType::ForStatement:
            traverse(ctx, node.as.for_stmt.start_expr, visitor);
            traverse(ctx, node.as.for_stmt.limit_expr, visitor);
            traverse(ctx, node.as.for_stmt.step_expr, visitor);
            traverse(ctx, node.as.for_stmt.body_block, visitor);
            break;
        case NodeType::GenericForStatement:
            for (uint32_t i = 0; i < node.as.generic_for.iter_count; ++i)
                traverse(ctx, ctx.block_statements[node.as.generic_for.first_iter + i], visitor);
            traverse(ctx, node.as.generic_for.body_block, visitor);
            break;
        case NodeType::DoStatement:
            traverse(ctx, node.as.do_stmt.body_block, visitor);
            break;
        case NodeType::TableConstructor:
            for (uint32_t i = 0; i < node.as.table_cons.count; ++i) {
                traverse(ctx, ctx.block_statements[node.as.table_cons.first_item + i * 2], visitor);
                traverse(ctx, ctx.block_statements[node.as.table_cons.first_item + i * 2 + 1], visitor);
            }
            break;
        case NodeType::TableAccess:
            traverse(ctx, node.as.table_access.table, visitor);
            traverse(ctx, node.as.table_access.key, visitor);
            break;
        case NodeType::ReturnStatement:
            for (uint32_t i = 0; i < node.as.return_stmt.value_count; ++i)
                traverse(ctx, ctx.block_statements[node.as.return_stmt.first_value + i], visitor);
            break;
        case NodeType::FunctionDef:
            traverse(ctx, node.as.func_def.body_block, visitor);
            break;
        case NodeType::ParenExpression:
            traverse(ctx, node.as.paren_expr.expr, visitor);
            break;
        default:
            break;
    }
}

}