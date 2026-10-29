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
#include <clang/Basic/LLVM.h>

#include <cstdint>

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

    // A third argument bounds how many code units of a string-literal needle
    // the search consults, while starts_with() measures the needle with
    // traits_type::length(), which stops at an embedded NUL. The rewritten
    // comparison keeps its meaning only when the proven count covers the
    // exact literal and the literal contains no NUL code unit. Unknown
    // counts, truncated counts, and unknown needles stay undiagnosed.
    bool hasEquivalentNeedleLength(CXXMemberCallExpr const& findCall, ASTContext& context)
    {
      auto const* needle = findCall.getArg(0);
      auto const* countArgument = findCall.getArg(2);

      if (needle == nullptr || countArgument == nullptr)
      {
        return false;
      }

      auto const* literal = dyn_cast<StringLiteral>(needle->IgnoreParenImpCasts());

      if (literal == nullptr)
      {
        return false;
      }

      auto evalResult = Expr::EvalResult{};

      if (!countArgument->EvaluateAsInt(evalResult, context))
      {
        return false;
      }

      auto const length = literal->getLength();

      // Exact equality rejects truncated, oversized, negative, and other
      // unproved counts.
      if (evalResult.Val.getInt() != static_cast<std::int64_t>(length))
      {
        return false;
      }

      // getCodeUnit() reads one code unit of any literal width, so a NUL in
      // narrow, UTF-16, and UTF-32 literals is caught the same way.
      for (std::uint32_t index = 0; index < length; ++index)
      {
        if (literal->getCodeUnit(index) == 0)
        {
          return false;
        }
      }

      return true;
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

    if (argumentCount >= 3 && !hasEquivalentNeedleLength(*findCall, *result.Context))
    {
      return;
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
