#include "UseRangesAnyOfCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
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

namespace clang::tidy::modernize
{
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

    if (auto const* calleeDecl = algoCall->getDirectCallee(); calleeDecl == nullptr)
    {
      return;
    }

    // std::ranges::find_if is a Niebloid, so the callee is actually the operator() of the functor object.
    // We should get the type of the functor (the 0th argument) to see if it's find_if or find_if_not.
    auto const* functorArg = algoCall->getArg(0)->IgnoreParenImpCasts();
    auto const* functorDecl = functorArg->getType()->getAsCXXRecordDecl();

    if (functorDecl == nullptr)
    {
      return;
    }

    // Aobus uses libstdc++ where functor names might be _Find_if or _Find_if_not.
    // Alternatively, we can check the declRefExpr of the 0th argument.
    auto const* declRef = dyn_cast<DeclRefExpr>(functorArg);

    if (declRef == nullptr)
    {
      return;
    }

    auto const algoName = declRef->getFoundDecl()->getQualifiedNameAsString();

    bool const isFindIf = (algoName == "std::ranges::__cpo::find_if" ||
                           algoName == "std::ranges::views::__cpo::find_if" || algoName == "std::ranges::find_if");
    bool const isFindIfNot =
      (algoName == "std::ranges::__cpo::find_if_not" || algoName == "std::ranges::views::__cpo::find_if_not" ||
       algoName == "std::ranges::find_if_not");

    if (!isFindIf && !isFindIfNot)
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const& langOpts = result.Context->getLangOpts();

    auto const rangeStr =
      Lexer::getSourceText(CharSourceRange::getTokenRange(rangeArg->getSourceRange()), sm, langOpts).str();
    auto const predStr =
      Lexer::getSourceText(CharSourceRange::getTokenRange(predArg->getSourceRange()), sm, langOpts).str();

    auto replacement = std::string{};
    auto diagnosticMsg = std::string{};

    if (isFindIf)
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
    else if (isFindIfNot)
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
