// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseEraseIfCheck.h"

#include "check/AstUtil.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>

#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    constexpr unsigned kMinArgsForRemoveCall = 3;
  } // namespace

  void UseEraseIfCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      cxxMemberCallExpr(
        callee(cxxMethodDecl(hasName("erase"))),
        on(declRefExpr(to(varDecl().bind("container")))),
        hasArgument(1,
                    expr(hasDescendant(cxxMemberCallExpr(callee(cxxMethodDecl(hasName("end"))),
                                                         on(declRefExpr(to(varDecl(equalsBoundNode("container"))))))))))
        .bind("erase_call"),
      this);
  }

  void UseEraseIfCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* eraseCall = result.Nodes.getNodeAs<CXXMemberCallExpr>("erase_call");
    auto const* container = result.Nodes.getNodeAs<VarDecl>("container");

    if (eraseCall == nullptr || container == nullptr || eraseCall->getNumArgs() < 2)
    {
      return;
    }

    auto const containerStr = container->getName().str();

    if (containerStr.empty())
    {
      return;
    }

    auto const* arg0 = aobus::stripImplicitNodes(eraseCall->getArg(0));
    auto const* removeCall = dyn_cast_or_null<CallExpr>(arg0);

    if (removeCall == nullptr || removeCall->getNumArgs() < kMinArgsForRemoveCall)
    {
      return;
    }

    auto const* calleeDecl = removeCall->getDirectCallee();

    if (calleeDecl == nullptr)
    {
      return;
    }

    auto const calleeName = calleeDecl->getQualifiedNameAsString();

    if (calleeName != "std::remove_if" && calleeName != "std::remove")
    {
      return;
    }

    auto const* arg0Remove = aobus::stripImplicitNodes(removeCall->getArg(0));
    auto const* arg1Remove = aobus::stripImplicitNodes(removeCall->getArg(1));

    auto const* beginCall = dyn_cast_or_null<CXXMemberCallExpr>(arg0Remove);
    auto const* endCall = dyn_cast_or_null<CXXMemberCallExpr>(arg1Remove);

    if (beginCall == nullptr || endCall == nullptr)
    {
      return;
    }

    if (auto const* beginMethod = beginCall->getMethodDecl();
        beginMethod == nullptr || beginMethod->getName() != "begin")
    {
      return;
    }

    if (auto const* endMethod = endCall->getMethodDecl(); endMethod == nullptr || endMethod->getName() != "end")
    {
      return;
    }

    if (beginCall->getNumArgs() != 0 || endCall->getNumArgs() != 0)
    {
      return;
    }

    if (!aobus::refersToVarDecl(beginCall->getImplicitObjectArgument(), *container) ||
        !aobus::refersToVarDecl(endCall->getImplicitObjectArgument(), *container))
    {
      return;
    }

    if (aobus::isInMacro(eraseCall->getSourceRange()))
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const& langOpts = result.Context->getLangOpts();
    auto const argStr = aobus::getExprSourceText(*removeCall->getArg(2), sm, langOpts);

    if (argStr.empty())
    {
      return;
    }

    if (calleeDecl->getName() == "remove_if")
    {
      auto const replacement = "std::erase_if(" + containerStr + ", " + argStr + ")";

      diag(eraseCall->getBeginLoc(), "use std::erase_if instead of erase-remove_if idiom")
        << FixItHint::CreateReplacement(eraseCall->getSourceRange(), replacement);
    }
    else
    {
      auto const replacement = "std::erase(" + containerStr + ", " + argStr + ")";

      diag(eraseCall->getBeginLoc(), "use std::erase instead of erase-remove idiom")
        << FixItHint::CreateReplacement(eraseCall->getSourceRange(), replacement);
    }
  }
} // namespace clang::tidy::readability
