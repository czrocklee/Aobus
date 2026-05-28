// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseRangesMinMaxCheck.h"

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

namespace clang::tidy::readability
{
namespace
{
constexpr unsigned kComparatorArgIndex = 3;
} // namespace

void UseRangesMinMaxCheck::registerMatchers(MatchFinder* finder)
{
  finder->addMatcher(
    cxxOperatorCallExpr(
      hasOverloadedOperatorName("*"),
      hasDescendant(
        callExpr(
          callee(functionDecl(anyOf(hasName("min_element"), hasName("max_element")))))
          .bind("call")),
      hasDescendant(
        cxxMemberCallExpr(
          callee(cxxMethodDecl(hasName("begin"))),
          on(declRefExpr(to(varDecl().bind("container")))))
          .bind("begin_call")))
      .bind("root"),
    this);
}

namespace
{
Expr const* stripImplicitNodes(Expr const* expr)
{
  if (expr == nullptr)
  {
    return nullptr;
  }

  Expr const* current = expr;

  while (true)
  {
    if (auto const* ice = dyn_cast<ImplicitCastExpr>(current))
    {
      current = ice->getSubExpr();
    }
    else if (auto const* cce = dyn_cast<CXXConstructExpr>(current))
    {
      if (cce->getNumArgs() == 1)
      {
        current = cce->getArg(0);
      }
      else
      {
        break;
      }
    }
    else if (auto const* mte = dyn_cast<MaterializeTemporaryExpr>(current))
    {
      current = mte->getSubExpr();
    }
    else
    {
      break;
    }
  }

  return current;
}

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

void UseRangesMinMaxCheck::check(MatchFinder::MatchResult const& result)
{
  auto const* root = result.Nodes.getNodeAs<CXXOperatorCallExpr>("root");
  auto const* call = result.Nodes.getNodeAs<CallExpr>("call");
  auto const* container = result.Nodes.getNodeAs<VarDecl>("container");
  auto const* beginCall = result.Nodes.getNodeAs<CXXMemberCallExpr>("begin_call");

  if (root == nullptr || call == nullptr || container == nullptr || beginCall == nullptr)
  {
    return;
  }

  auto const* calleeDecl = call->getDirectCallee();

  if (calleeDecl == nullptr)
  {
    return;
  }

  auto const calleeName = calleeDecl->getName();
  bool const isMin = calleeName == "min_element";

  if (!isMin && calleeName != "max_element")
  {
    return;
  }

  if (beginCall->getNumArgs() != 0)
  {
    return;
  }

  if (call->getNumArgs() < 2)
  {
    return;
  }

  auto const* arg0 = stripImplicitNodes(call->getArg(0));
  auto const* arg1 = stripImplicitNodes(call->getArg(1));

  auto const* arg0BeginCall = dyn_cast_or_null<CXXMemberCallExpr>(arg0);
  auto const* arg1EndCall = dyn_cast_or_null<CXXMemberCallExpr>(arg1);

  if (arg0BeginCall == nullptr || arg1EndCall == nullptr)
  {
    return;
  }

  if (auto const* beginMethod = arg0BeginCall->getMethodDecl();
      beginMethod == nullptr || beginMethod->getName() != "begin")
  {
    return;
  }

  if (auto const* endMethod = arg1EndCall->getMethodDecl();
      endMethod == nullptr || endMethod->getName() != "end")
  {
    return;
  }

  if (arg0BeginCall->getNumArgs() != 0 || arg1EndCall->getNumArgs() != 0)
  {
    return;
  }

  auto const& sm = *result.SourceManager;
  auto const& langOpts = result.Context->getLangOpts();
  auto const containerStr = container->getName().str();

  if (getSource(arg0BeginCall->getImplicitObjectArgument(), sm, langOpts) != containerStr
      || getSource(arg1EndCall->getImplicitObjectArgument(), sm, langOpts) != containerStr)
  {
    return;
  }

  auto const* const funcName = isMin ? "std::ranges::min" : "std::ranges::max";

  auto replacement = std::string{};

  if (call->getNumArgs() >= kComparatorArgIndex)
  {
    auto const compStr = getSource(call->getArg(2), sm, langOpts);

    if (compStr.empty())
    {
      return;
    }

    replacement = std::string{funcName} + "(" + containerStr + ", " + compStr + ")";
  }
  else
  {
    replacement = std::string{funcName} + "(" + containerStr + ")";
  }

  diag(root->getBeginLoc(), "use %0 instead of dereferencing %select{min|max}1_element")
      << funcName << (isMin ? 0 : 1)
    << FixItHint::CreateReplacement(root->getSourceRange(), replacement);
}
} // namespace clang::tidy::readability
