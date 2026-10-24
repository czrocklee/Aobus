// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ImplicitBoolConversionInInitCheck.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>

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

    // Diagnose only conversions with a known explicit comparison form.
    auto comparison = StringRef{};

    switch (cast->getCastKind())
    {
      case CK_PointerToBoolean:
      case CK_MemberPointerToBoolean: comparison = "!= nullptr"; break;
      case CK_IntegralToBoolean:
      case CK_FloatingToBoolean: comparison = "!= 0"; break;
      default: return;
    }

    diag(bareVar->getBeginLoc(),
         "implicit conversion to bool in if-statement condition is forbidden; use an explicit '%0' comparison instead")
      << comparison;
  }
} // namespace clang::tidy::aobus
