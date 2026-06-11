// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ForbidRawThrowCheck.h"

#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void ForbidRawThrowCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      cxxThrowExpr(unless(hasAncestor(functionDecl(hasAnyName("throwException", "ao::throwException"))))).bind("throw"),
      this);
  }

  void ForbidRawThrowCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* throwExpr = result.Nodes.getNodeAs<CXXThrowExpr>("throw");

    if (throwExpr == nullptr)
    {
      return;
    }

    if (throwExpr->getSubExpr() == nullptr)
    {
      return;
    }

    SourceLocation const loc = throwExpr->getThrowLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    diag(loc, "do not use raw 'throw' expression; use 'ao::throwException' instead");
  }
} // namespace clang::tidy::readability
