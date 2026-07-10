// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/OptionalNamingAndUsageCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Support/Casting.h>

#include <cstdint>

namespace
{
  constexpr std::int32_t kMaxParentWalkDepth = 12;

  bool isTransparentExprParent(clang::Expr const* expr)
  {
    return llvm::isa<clang::ParenExpr>(expr) || llvm::isa<clang::ImplicitCastExpr>(expr) ||
           llvm::isa<clang::ExprWithCleanups>(expr) || llvm::isa<clang::MaterializeTemporaryExpr>(expr) ||
           llvm::isa<clang::CXXBindTemporaryExpr>(expr);
  }

  bool isContextualBoolUse(clang::Expr const* expr, clang::ASTContext& context)
  {
    clang::Stmt const* current = expr;

    for (std::int32_t depth = 0; depth < kMaxParentWalkDepth; ++depth)
    {
      auto parents = context.getParents(*current);

      if (parents.empty())
      {
        return false;
      }

      auto const& parent = parents[0];

      if (auto const* parentExpr = parent.get<clang::Expr>(); parentExpr != nullptr)
      {
        if (isTransparentExprParent(parentExpr))
        {
          current = parentExpr;
          continue;
        }

        if (auto const* unary = llvm::dyn_cast<clang::UnaryOperator>(parentExpr);
            unary != nullptr && unary->getOpcode() == clang::UO_LNot)
        {
          return true;
        }

        if (auto const* binary = llvm::dyn_cast<clang::BinaryOperator>(parentExpr);
            binary != nullptr && binary->isLogicalOp())
        {
          return true;
        }

        if (auto const* conditional = llvm::dyn_cast<clang::AbstractConditionalOperator>(parentExpr);
            conditional != nullptr && conditional->getCond() == current)
        {
          return true;
        }

        return false;
      }

      if (auto const* ifStmt = parent.get<clang::IfStmt>(); ifStmt != nullptr)
      {
        return ifStmt->getCond() == current;
      }

      if (auto const* whileStmt = parent.get<clang::WhileStmt>(); whileStmt != nullptr)
      {
        return whileStmt->getCond() == current;
      }

      if (auto const* doStmt = parent.get<clang::DoStmt>(); doStmt != nullptr)
      {
        return doStmt->getCond() == current;
      }

      if (auto const* forStmt = parent.get<clang::ForStmt>(); forStmt != nullptr)
      {
        return forStmt->getCond() == current;
      }

      return false;
    }

    return false;
  }
} // namespace

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  void OptionalNamingAndUsageCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    // Robustly match std::optional by looking for the template specialization
    auto isOptionalType = hasType(qualType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(classTemplateSpecializationDecl(hasName("::std::optional")))))));

    // Rule 1: Match all variables and fields of type std::optional
    finder->addMatcher(declaratorDecl(isOptionalType).bind("optional_decl"), this);

    // Rule 2: Match .has_value() only when the receiver is a named optional variable or field.
    // Temporary optional expressions such as reader.get(id).has_value() are intentionally allowed.
    finder->addMatcher(
      cxxMemberCallExpr(
        on(expr(isOptionalType,
                ignoringParenImpCasts(anyOf(declRefExpr(to(varDecl())).bind("optional_var_ref"),
                                            memberExpr(member(fieldDecl())).bind("optional_field_ref"))))),
        callee(cxxMethodDecl(hasName("has_value"))))
        .bind("has_value_call"),
      this);

    // Rule 3: Match explicit boolean casts from std::optional. Contextual boolean
    // positions should use the optional directly; materialized bool values should
    // use .has_value().
    finder->addMatcher(cxxStaticCastExpr(hasDestinationType(booleanType()),
                                         hasSourceExpression(ignoringParenImpCasts(
                                           cxxMemberCallExpr(on(expr(isOptionalType).bind("optional_bool_cast_source")),
                                                             callee(cxxConversionDecl(returns(booleanType())))))))
                         .bind("optional_bool_static_cast"),
                       this);
  }

  void OptionalNamingAndUsageCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    // Check Rule 1: Naming
    if (auto const* decl = result.Nodes.getNodeAs<DeclaratorDecl>("optional_decl"); decl != nullptr)
    {
      if (sm.isInSystemHeader(decl->getLocation()) || decl->getLocation().isMacroID())
      {
        return;
      }

      StringRef name = decl->getName();

      while (name.consume_front("_"))
      {
      }

      if (!name.empty() && !name.starts_with_insensitive("opt"))
      {
        diag(decl->getLocation(), "std::optional %0 %1 should start with 'opt' to indicate it is optional")
          << (llvm::isa<FieldDecl>(decl) ? "member" : "variable") << decl;
      }
    }

    // Check Rule 2: .has_value() usage on named optionals
    if (auto const* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("has_value_call"); call != nullptr)
    {
      if (!isContextualBoolUse(call, *result.Context))
      {
        return;
      }

      if (auto const beginLoc = call->getBeginLoc();
          sm.isInSystemHeader(sm.getSpellingLoc(beginLoc)) || sm.isMacroBodyExpansion(beginLoc))
      {
        return;
      }

      diag(sm.getSpellingLoc(call->getExprLoc()),
           "prefer concise boolean conversion for named optional variables and fields");
    }

    // Check Rule 3: static_cast<bool>(optional)
    if (auto const* cast = result.Nodes.getNodeAs<CXXStaticCastExpr>("optional_bool_static_cast"); cast != nullptr)
    {
      if (auto const beginLoc = cast->getBeginLoc();
          sm.isInSystemHeader(sm.getSpellingLoc(beginLoc)) || sm.isMacroBodyExpansion(beginLoc))
      {
        return;
      }

      diag(sm.getSpellingLoc(cast->getExprLoc()),
           "avoid static_cast<bool> for std::optional; use the optional directly in boolean contexts and "
           ".has_value() when materializing bool");
    }
  }
} // namespace clang::tidy::readability
