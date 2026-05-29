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

    // Determine if it's a pointer or integer conversion.
    bool isPointer = false;

    if (cast->getCastKind() == CK_PointerToBoolean)
    {
      isPointer = true;
    }
    else if (cast->getCastKind() == CK_IntegralToBoolean || cast->getCastKind() == CK_FloatingToBoolean)
    {
      isPointer = false;
    }
    else
    {
      // Only care about pointer and integer conversions for now, maybe others if needed.
    }

    char const* replacement = isPointer ? " != nullptr" : " != 0";

    auto diagnostic =
      diag(bareVar->getBeginLoc(),
           "implicit conversion to bool in if-statement condition is forbidden; use explicit comparison instead");

    // We insert ` != nullptr` or ` != 0` immediately after the variable name in the condition.
    SourceLocation const endLoc =
      Lexer::getLocForEndOfToken(bareVar->getEndLoc(), 0, *result.SourceManager, result.Context->getLangOpts());

    if (endLoc.isValid())
    {
      diagnostic << FixItHint::CreateInsertion(endLoc, replacement);
    }
  }
} // namespace clang::tidy::aobus
