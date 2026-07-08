// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseRangesContainsCheck.h"

#include "check/AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <llvm/ADT/StringRef.h>

#include <cstdint>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    // Number of explicitly written arguments of the range overload: CPO object
    // + range + value for the operator() form, range + value for a plain call.
    constexpr std::uint32_t kCpoRangeOverloadArgs = 3;
    constexpr std::uint32_t kPlainRangeOverloadArgs = 2;

    // Counts arguments the caller actually wrote, skipping defaulted ones such
    // as the trailing projection parameter of std::ranges algorithms.
    std::uint32_t countExplicitArgs(CallExpr const& call)
    {
      std::uint32_t count = 0;

      for (Expr const* argument : call.arguments())
      {
        if (!isa<CXXDefaultArgExpr>(argument))
        {
          ++count;
        }
      }

      return count;
    }

    // Verifies via the AST that algoCall invokes std::ranges::find or
    // std::ranges::count on the (range, value) overload. Rejects same-named
    // functions from other namespaces and the iterator-pair overloads, whose
    // first argument is not a range.
    bool isRangesAlgo(CallExpr const& algoCall, llvm::StringRef const expectedName)
    {
      if (auto const* opCall = dyn_cast<CXXOperatorCallExpr>(&algoCall); opCall != nullptr)
      {
        return countExplicitArgs(*opCall) == kCpoRangeOverloadArgs && aobus::getRangesCpoName(*opCall) == expectedName;
      }

      if (countExplicitArgs(algoCall) != kPlainRangeOverloadArgs)
      {
        return false;
      }

      auto const* calleeDecl = algoCall.getDirectCallee();

      if (calleeDecl == nullptr || calleeDecl->getIdentifier() == nullptr || calleeDecl->getName() != expectedName)
      {
        return false;
      }

      return llvm::StringRef{calleeDecl->getQualifiedNameAsString()}.starts_with("std::ranges::");
    }

    Expr const* getComparisonExpr(MatchFinder::MatchResult const& result, bool isFindGroup)
    {
      if (isFindGroup)
      {
        auto const* findEq = result.Nodes.getNodeAs<Expr>("find_eq");

        return findEq != nullptr ? findEq : result.Nodes.getNodeAs<Expr>("find_ne");
      }

      if (auto const* countEq = result.Nodes.getNodeAs<Expr>("count_eq"); countEq != nullptr)
      {
        return countEq;
      }

      if (auto const* countNe = result.Nodes.getNodeAs<Expr>("count_ne"); countNe != nullptr)
      {
        return countNe;
      }

      return result.Nodes.getNodeAs<Expr>("count_gt");
    }
  } // namespace

  void UseRangesContainsCheck::registerMatchers(MatchFinder* finder)
  {
    auto rangeMatcher = expr().bind("range");
    auto valueMatcher = expr().bind("val");

    auto algoCall = anyOf(
      callExpr(unless(cxxOperatorCallExpr()), hasArgument(0, rangeMatcher), hasArgument(1, valueMatcher)),
      cxxOperatorCallExpr(hasOverloadedOperatorName("()"), hasArgument(1, rangeMatcher), hasArgument(2, valueMatcher)));
    auto boundAlgoCall = expr(algoCall).bind("algo_call");

    auto endCall = anyOf(callExpr().bind("end_call"), cxxMemberCallExpr(callee(cxxMethodDecl(hasName("end")))));

    auto algoCmpEq = binaryOperation(hasOperatorName("=="),
                                     hasOperands(ignoringParenImpCasts(boundAlgoCall), ignoringParenImpCasts(endCall)))
                       .bind("find_eq");

    auto algoCmpNe = binaryOperation(hasOperatorName("!="),
                                     hasOperands(ignoringParenImpCasts(boundAlgoCall), ignoringParenImpCasts(endCall)))
                       .bind("find_ne");

    finder->addMatcher(algoCmpEq, this);
    finder->addMatcher(algoCmpNe, this);

    auto countCmpEq = binaryOperation(hasOperatorName("=="),
                                      hasOperands(ignoringParenImpCasts(boundAlgoCall),
                                                  ignoringParenImpCasts(integerLiteral(equals(0)))))
                        .bind("count_eq");

    auto countCmpNe = binaryOperation(hasOperatorName("!="),
                                      hasOperands(ignoringParenImpCasts(boundAlgoCall),
                                                  ignoringParenImpCasts(integerLiteral(equals(0)))))
                        .bind("count_ne");

    auto countCmpGt = binaryOperation(hasOperatorName(">"),
                                      hasOperands(ignoringParenImpCasts(boundAlgoCall),
                                                  ignoringParenImpCasts(integerLiteral(equals(0)))))
                        .bind("count_gt");

    finder->addMatcher(countCmpEq, this);
    finder->addMatcher(countCmpNe, this);
    finder->addMatcher(countCmpGt, this);
  }

  void UseRangesContainsCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* algoCall = result.Nodes.getNodeAs<CallExpr>("algo_call");
    auto const* range = result.Nodes.getNodeAs<Expr>("range");
    auto const* val = result.Nodes.getNodeAs<Expr>("val");

    if (algoCall == nullptr || range == nullptr || val == nullptr)
    {
      return;
    }

    auto const* findEq = result.Nodes.getNodeAs<Expr>("find_eq");
    auto const* countEq = result.Nodes.getNodeAs<Expr>("count_eq");

    bool const isFindGroup = findEq != nullptr || result.Nodes.getNodeAs<Expr>("find_ne") != nullptr;
    auto const* cmp = getComparisonExpr(result, isFindGroup);

    if (cmp == nullptr)
    {
      return;
    }

    if (!isRangesAlgo(*algoCall, isFindGroup ? "find" : "count"))
    {
      return;
    }

    if (isFindGroup)
    {
      auto const* endCall = result.Nodes.getNodeAs<CallExpr>("end_call");

      if (endCall == nullptr || !aobus::isEndCall(*endCall))
      {
        return;
      }
    }

    if (aobus::isWithinRewrittenOperator(*cmp, *result.Context))
    {
      return;
    }

    if (aobus::isInMacro(cmp->getSourceRange()))
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const& langOpts = result.Context->getLangOpts();

    auto const rangeStr = aobus::getExprSourceText(*range, sm, langOpts);
    auto valStr = aobus::getExprSourceText(*val, sm, langOpts);

    if (rangeStr.empty() || valStr.empty())
    {
      return;
    }

    if (isFindGroup && !aobus::verifyEndObject(*result.Nodes.getNodeAs<CallExpr>("end_call"), rangeStr, sm, langOpts))
    {
      return;
    }

    if (isa<StringLiteral>(val->IgnoreParenImpCasts()))
    {
      valStr = "std::string_view{" + valStr + "}";
    }

    auto replacement = "std::ranges::contains(" + rangeStr + ", " + valStr + ")";

    // find == end and count == 0 both assert absence; the other comparison
    // forms assert presence.
    if (findEq != nullptr || countEq != nullptr)
    {
      replacement = "!" + replacement;
    }

    auto const* message = isFindGroup ? "use std::ranges::contains instead of std::ranges::find != end"
                                      : "use std::ranges::contains instead of std::ranges::count > 0";

    diag(cmp->getBeginLoc(), message) << FixItHint::CreateReplacement(cmp->getSourceRange(), replacement);
  }
} // namespace clang::tidy::readability
