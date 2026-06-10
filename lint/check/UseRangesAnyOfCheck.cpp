// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "UseRangesAnyOfCheck.h"

#include "AstUtil.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>

#include <cstdint>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::modernize
{
  namespace
  {
    enum class AlgoKind : std::uint8_t
    {
      None,
      FindIf,
      FindIfNot
    };

    AlgoKind getAlgoKind(CXXOperatorCallExpr const* algoCall)
    {
      auto const algoName = aobus::getRangesCpoName(*algoCall);

      if (algoName == "find_if")
      {
        return AlgoKind::FindIf;
      }

      if (algoName == "find_if_not")
      {
        return AlgoKind::FindIfNot;
      }

      return AlgoKind::None;
    }
  } // namespace

  void UseRangesAnyOfCheck::registerMatchers(MatchFinder* finder)
  {
    if (!getLangOpts().CPlusPlus20)
    {
      return;
    }

    auto rangeArg = expr().bind("range");
    auto predArg = expr().bind("pred");

    auto algoCall =
      cxxOperatorCallExpr(hasOverloadedOperatorName("()"), hasArgument(1, rangeArg), hasArgument(2, predArg));
    auto boundAlgoCall = expr(algoCall).bind("algo_call");

    auto endCall = anyOf(callExpr().bind("end_call"), cxxMemberCallExpr(callee(cxxMethodDecl(hasName("end")))));

    auto findEqEnd = binaryOperation(hasOperatorName("=="),
                                     hasOperands(ignoringParenImpCasts(boundAlgoCall), ignoringParenImpCasts(endCall)))
                       .bind("find_eq");

    auto findNeEnd = binaryOperation(hasOperatorName("!="),
                                     hasOperands(ignoringParenImpCasts(boundAlgoCall), ignoringParenImpCasts(endCall)))
                       .bind("find_ne");

    finder->addMatcher(findEqEnd, this);
    finder->addMatcher(findNeEnd, this);
  }

  void UseRangesAnyOfCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* eqOp = result.Nodes.getNodeAs<Expr>("find_eq");
    auto const* neOp = result.Nodes.getNodeAs<Expr>("find_ne");
    auto const* match = (eqOp != nullptr) ? eqOp : neOp;

    if (match == nullptr)
    {
      return;
    }

    auto const* algoCall = result.Nodes.getNodeAs<CXXOperatorCallExpr>("algo_call");

    if (algoCall == nullptr)
    {
      return;
    }

    auto const* rangeArg = result.Nodes.getNodeAs<Expr>("range");
    auto const* predArg = result.Nodes.getNodeAs<Expr>("pred");

    if (rangeArg == nullptr || predArg == nullptr)
    {
      return;
    }

    auto const* endCall = result.Nodes.getNodeAs<CallExpr>("end_call");

    if (endCall == nullptr)
    {
      return;
    }

    auto const algoKind = getAlgoKind(algoCall);

    if (algoKind == AlgoKind::None)
    {
      return;
    }

    if (!aobus::isEndCall(*endCall))
    {
      return;
    }

    if (aobus::isWithinRewrittenOperator(*match, *result.Context))
    {
      return;
    }

    if (aobus::isInMacro(match->getSourceRange()))
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const& langOpts = result.Context->getLangOpts();

    auto const rangeStr = aobus::getExprSourceText(*rangeArg, sm, langOpts);

    if (!aobus::verifyEndObject(*endCall, rangeStr, sm, langOpts))
    {
      return;
    }

    auto const predStr = aobus::getExprSourceText(*predArg, sm, langOpts);

    auto replacement = std::string{};
    auto diagnosticMsg = std::string{};

    if (algoKind == AlgoKind::FindIf)
    {
      if (neOp != nullptr)
      {
        // find_if != end() -> any_of
        replacement = "std::ranges::any_of(" + rangeStr + ", " + predStr + ")";
        diagnosticMsg = "use std::ranges::any_of instead of std::ranges::find_if != end()";
      }
      else
      {
        // find_if == end() -> none_of
        replacement = "std::ranges::none_of(" + rangeStr + ", " + predStr + ")";
        diagnosticMsg = "use std::ranges::none_of instead of std::ranges::find_if == end()";
      }
    }
    else if (algoKind == AlgoKind::FindIfNot)
    {
      if (eqOp != nullptr)
      {
        // find_if_not == end() -> all_of
        replacement = "std::ranges::all_of(" + rangeStr + ", " + predStr + ")";
        diagnosticMsg = "use std::ranges::all_of instead of std::ranges::find_if_not == end()";
      }
      else
      {
        // find_if_not != end() -> !all_of (less common, but handled as !all_of)
        replacement = "!std::ranges::all_of(" + rangeStr + ", " + predStr + ")";
        diagnosticMsg = "use !std::ranges::all_of instead of std::ranges::find_if_not != end()";
      }
    }

    diag(match->getBeginLoc(), diagnosticMsg) << FixItHint::CreateReplacement(match->getSourceRange(), replacement);
  }
} // namespace clang::tidy::modernize
