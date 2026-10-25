// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseStartsWithCheck.h"

#include "check/AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void UseStartsWithCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      binaryOperator(
        anyOf(hasOperatorName("=="), hasOperatorName("!=")),
        hasEitherOperand(ignoringParenImpCasts(
          cxxMemberCallExpr(callee(cxxMethodDecl(hasName("find"))), on(expr().bind("obj"))).bind("find_call"))),
        hasEitherOperand(ignoringParenImpCasts(integerLiteral(equals(0)))))
        .bind("root"),
      this);
  }

  namespace
  {
    // Only std::basic_string and std::basic_string_view are known to pair a
    // position-returning find() with a starts_with() member; rewriting any
    // other type's find() would reference a member that may not exist.
    bool hasStartsWithSemantics(CXXMemberCallExpr const& findCall)
    {
      auto const* method = findCall.getMethodDecl();

      if (method == nullptr)
      {
        return false;
      }

      auto const className = method->getParent()->getQualifiedNameAsString();

      return className == "std::basic_string" || className == "std::basic_string_view";
    }
  } // namespace

  void UseStartsWithCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* root = result.Nodes.getNodeAs<BinaryOperator>("root");
    auto const* findCall = result.Nodes.getNodeAs<CXXMemberCallExpr>("find_call");

    if (auto const* obj = result.Nodes.getNodeAs<Expr>("obj"); root == nullptr || findCall == nullptr || obj == nullptr)
    {
      return;
    }

    if (!hasStartsWithSemantics(*findCall))
    {
      return;
    }

    unsigned const argumentCount = findCall->getNumArgs();

    if (argumentCount == 0)
    {
      return;
    }

    if (argumentCount >= 2)
    {
      auto const* positionArgument = findCall->getArg(1);

      if (auto evalResult = Expr::EvalResult{}; positionArgument == nullptr ||
                                                !positionArgument->EvaluateAsInt(evalResult, *result.Context) ||
                                                evalResult.Val.getInt() != 0)
      {
        return;
      }
    }

    if (aobus::isInMacro(root->getSourceRange()))
    {
      return;
    }

    // == 0 is a prefix; != 0 is not. Recommend the starts_with() member.
    // There is no free std::starts_with.
    auto const* message = root->getOpcode() == BO_NE ? "use !starts_with() member instead of find != 0"
                                                     : "use starts_with() member instead of find == 0";

    diag(root->getBeginLoc(), message);
  }
} // namespace clang::tidy::readability
