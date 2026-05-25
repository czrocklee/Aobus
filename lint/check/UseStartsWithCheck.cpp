#include "check/UseStartsWithCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
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
void UseStartsWithCheck::registerMatchers(MatchFinder* finder)
{
  finder->addMatcher(
    binaryOperator(
      anyOf(hasOperatorName("=="), hasOperatorName("!=")),
      hasEitherOperand(
        ignoringParenImpCasts(
          cxxMemberCallExpr(
            callee(cxxMethodDecl(hasName("find"))),
            on(expr().bind("obj")))
            .bind("find_call"))),
      hasEitherOperand(ignoringParenImpCasts(integerLiteral(equals(0)))))
      .bind("root"),
    this);
}

namespace
{
std::string getSource(Expr const* expr, SourceManager const& sm, LangOptions const& langOpts)
{
  if (expr == nullptr)
  {
    return {};
  }

  return Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), sm, langOpts)
    .str();
}
} // namespace

void UseStartsWithCheck::check(MatchFinder::MatchResult const& result)
{
  auto const* root = result.Nodes.getNodeAs<BinaryOperator>("root");
  auto const* findCall = result.Nodes.getNodeAs<CXXMemberCallExpr>("find_call");
  auto const* obj = result.Nodes.getNodeAs<Expr>("obj");

  if (root == nullptr || findCall == nullptr || obj == nullptr)
  {
    return;
  }

  unsigned const numArgs = findCall->getNumArgs();

  if (numArgs == 0)
  {
    return;
  }

  if (numArgs >= 2)
  {
    auto const* posArg = findCall->getArg(1);

    if (auto evalResult = Expr::EvalResult{}; posArg == nullptr
        || !posArg->EvaluateAsInt(evalResult, *result.Context)
        || evalResult.Val.getInt() != 0)
    {
      return;
    }
  }

  auto const& sm = *result.SourceManager;
  auto const& langOpts = result.Context->getLangOpts();

  auto const objStr = getSource(obj, sm, langOpts);
  auto const argStr = getSource(findCall->getArg(0), sm, langOpts);

  if (objStr.empty() || argStr.empty())
  {
    return;
  }

  bool const isNegated = root->getOpcode() == BO_NE;

  auto replacement = std::string{};

  if (isNegated)
  {
    replacement = "!" + objStr + ".starts_with(" + argStr + ")";
  }
  else
  {
    replacement = objStr + ".starts_with(" + argStr + ")";
  }

  diag(root->getBeginLoc(), "use std::starts_with instead of find-and-compare pattern")
    << FixItHint::CreateReplacement(root->getSourceRange(), replacement);
}
} // namespace clang::tidy::readability
