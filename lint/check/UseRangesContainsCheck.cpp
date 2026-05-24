#include "check/UseRangesContainsCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>

#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void UseRangesContainsCheck::registerMatchers(MatchFinder* finder)
  {
    auto rangeArg = expr().bind("range");
    auto valArg = expr().bind("val");

    auto algoCall =
      anyOf(callExpr(unless(cxxOperatorCallExpr()), hasArgument(0, rangeArg), hasArgument(1, valArg)),
            cxxOperatorCallExpr(hasOverloadedOperatorName("()"), hasArgument(1, rangeArg), hasArgument(2, valArg)));
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
    auto const* endCall = result.Nodes.getNodeAs<CallExpr>("end_call");
    auto const* range = result.Nodes.getNodeAs<Expr>("range");
    auto const* val = result.Nodes.getNodeAs<Expr>("val");

    if (algoCall == nullptr || range == nullptr || val == nullptr)
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const& langOpts = result.Context->getLangOpts();

    auto const getSourceText = [&](Expr const* exprArg) -> std::string
    {
      if (exprArg == nullptr)
      {
        return "";
      }

      return Lexer::getSourceText(CharSourceRange::getTokenRange(exprArg->getSourceRange()), sm, langOpts).str();
    };

    auto const* calleeExpr = (isa<CXXOperatorCallExpr>(algoCall)) ? algoCall->getArg(0) : algoCall->getCallee();
    auto const calleeText = getSourceText(calleeExpr);

    bool const isFind = calleeText.find("find") != std::string::npos;
    bool const isCount = calleeText.find("count") != std::string::npos;

    if (!isFind && !isCount)
    {
      return;
    }

    if (endCall != nullptr)
    {
      auto const* endCalleeExpr = (isa<CXXOperatorCallExpr>(endCall)) ? endCall->getArg(0) : endCall->getCallee();

      if (auto const endCalleeText = getSourceText(endCalleeExpr); endCalleeText.find("end") == std::string::npos)
      {
        return;
      }
    }

    auto const rangeStr = getSourceText(range);
    auto const valStr = getSourceText(val);

    if (rangeStr.empty() || valStr.empty())
    {
      return;
    }

    auto const replacementBase = "std::ranges::contains(" + rangeStr + ", " + valStr + ")";

    handleFindReplacement(result, replacementBase, isFind);
    handleCountReplacement(result, replacementBase, isCount);
  }

  void UseRangesContainsCheck::handleFindReplacement(MatchFinder::MatchResult const& result,
                                                     std::string const& replacementBase,
                                                     bool isFind)
  {
    auto const* findEq = result.Nodes.getNodeAs<Expr>("find_eq");
    auto const* findNe = result.Nodes.getNodeAs<Expr>("find_ne");

    if (findEq == nullptr && findNe == nullptr)
    {
      return;
    }

    if (!isFind)
    {
      return;
    }

    auto replacement = replacementBase;
    auto const* cmp = (findEq != nullptr) ? findEq : findNe;

    if (findEq != nullptr)
    {
      replacement = "!" + replacement;
    }

    diag(cmp->getBeginLoc(), "use std::ranges::contains instead of std::ranges::find != end")
      << FixItHint::CreateReplacement(cmp->getSourceRange(), replacement);
  }

  void UseRangesContainsCheck::handleCountReplacement(MatchFinder::MatchResult const& result,
                                                      std::string const& replacementBase,
                                                      bool isCount)
  {
    auto const* countEq = result.Nodes.getNodeAs<Expr>("count_eq");
    auto const* countNe = result.Nodes.getNodeAs<Expr>("count_ne");
    auto const* countGt = result.Nodes.getNodeAs<Expr>("count_gt");

    if (countEq == nullptr && countNe == nullptr && countGt == nullptr)
    {
      return;
    }

    if (!isCount)
    {
      return;
    }

    auto replacement = replacementBase;
    Expr const* cmp = nullptr;

    if (countEq != nullptr)
    {
      cmp = countEq;
      replacement = "!" + replacement;
    }
    else
    {
      cmp = (countNe != nullptr) ? countNe : countGt;
    }

    diag(cmp->getBeginLoc(), "use std::ranges::contains instead of std::ranges::count > 0")
      << FixItHint::CreateReplacement(cmp->getSourceRange(), replacement);
  }
} // namespace clang::tidy::readability
