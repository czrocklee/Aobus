// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseEraseIfCheck.h"

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
        if (auto const* ice = dyn_cast<ImplicitCastExpr>(current); ice != nullptr)
        {
          current = ice->getSubExpr();
        }
        else if (auto const* cce = dyn_cast<CXXConstructExpr>(current); cce != nullptr)
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
        else if (auto const* mte = dyn_cast<MaterializeTemporaryExpr>(current); mte != nullptr)
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

      return Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), sm, langOpts).str();
    }
  } // namespace

  void UseEraseIfCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* eraseCall = result.Nodes.getNodeAs<CXXMemberCallExpr>("erase_call");
    auto const* container = result.Nodes.getNodeAs<VarDecl>("container");

    if (eraseCall == nullptr || container == nullptr || eraseCall->getNumArgs() < 2)
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const& langOpts = result.Context->getLangOpts();

    auto const containerStr = container->getName().str();

    if (containerStr.empty())
    {
      return;
    }

    auto const* arg0 = stripImplicitNodes(eraseCall->getArg(0));
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

    auto const calleeName = calleeDecl->getName();

    if (calleeName != "remove_if" && calleeName != "remove")
    {
      return;
    }

    auto const* arg0Remove = stripImplicitNodes(removeCall->getArg(0));
    auto const* arg1Remove = stripImplicitNodes(removeCall->getArg(1));

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

    if (getSource(beginCall->getImplicitObjectArgument(), sm, langOpts) != containerStr ||
        getSource(endCall->getImplicitObjectArgument(), sm, langOpts) != containerStr)
    {
      return;
    }

    if (calleeName == "remove_if")
    {
      auto const predStr = getSource(removeCall->getArg(2), sm, langOpts);

      if (predStr.empty())
      {
        return;
      }

      auto const replacement = "std::erase_if(" + containerStr + ", " + predStr + ")";

      diag(eraseCall->getBeginLoc(), "use std::erase_if instead of erase-remove_if idiom")
        << FixItHint::CreateReplacement(eraseCall->getSourceRange(), replacement);
    }
    else
    {
      auto const valueStr = getSource(removeCall->getArg(2), sm, langOpts);

      if (valueStr.empty())
      {
        return;
      }

      auto const replacement = "std::erase(" + containerStr + ", " + valueStr + ")";

      diag(eraseCall->getBeginLoc(), "use std::erase instead of erase-remove idiom")
        << FixItHint::CreateReplacement(eraseCall->getSourceRange(), replacement);
    }
  }
} // namespace clang::tidy::readability
