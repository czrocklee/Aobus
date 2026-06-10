// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ImplicitBoolConversionInInitCheck.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>

using namespace clang::ast_matchers;

namespace clang::tidy::aobus
{
  ImplicitBoolConversionInInitCheck::ImplicitBoolConversionInInitCheck(StringRef name, ClangTidyContext* context)
    : ClangTidyCheck{name, context}
  {
  }

  void ImplicitBoolConversionInInitCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      ifStmt(hasInitStatement(declStmt(hasSingleDecl(varDecl().bind("initVar")))),
             hasCondition(implicitCastExpr(hasImplicitDestinationType(booleanType()),
                                           hasSourceExpression(ignoringParenImpCasts(
                                             declRefExpr(to(varDecl(equalsBoundNode("initVar")))).bind("bareVar"))))
                            .bind("cast")))
        .bind("ifStmt"),
      this);
  }

  void ImplicitBoolConversionInInitCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* cast = result.Nodes.getNodeAs<ImplicitCastExpr>("cast");
    auto const* bareVar = result.Nodes.getNodeAs<DeclRefExpr>("bareVar");

    if (cast == nullptr || bareVar == nullptr)
    {
      return;
    }

    // Pick the explicit comparison for this conversion kind. Unhandled kinds
    // bail out instead of falling through to a potentially wrong suggestion.
    char const* replacement = nullptr;

    switch (cast->getCastKind())
    {
      case CK_PointerToBoolean:
      case CK_MemberPointerToBoolean: replacement = " != nullptr"; break;
      case CK_IntegralToBoolean:
      case CK_FloatingToBoolean: replacement = " != 0"; break;
      default: return;
    }

    auto diagnostic =
      diag(bareVar->getBeginLoc(),
           "implicit conversion to bool in if-statement condition is forbidden; use explicit comparison instead");

    // We insert ` != nullptr` or ` != 0` immediately after the variable name in the condition.
    SourceLocation const endLoc =
      Lexer::getLocForEndOfToken(bareVar->getEndLoc(), 0, *result.SourceManager, result.Context->getLangOpts());

    // A variable spelled inside a macro cannot take the insertion; the FixIt
    // would edit the macro definition.
    if (endLoc.isValid() && !bareVar->getEndLoc().isMacroID())
    {
      diagnostic << FixItHint::CreateInsertion(endLoc, replacement);
    }
  }
} // namespace clang::tidy::aobus
