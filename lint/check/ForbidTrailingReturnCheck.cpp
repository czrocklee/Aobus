// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ForbidTrailingReturnCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void ForbidTrailingReturnCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(functionDecl(hasTrailingReturn(), unless(isImplicit())).bind("func"), this);
  }

  void ForbidTrailingReturnCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* func = result.Nodes.getNodeAs<FunctionDecl>("func");

    if (func == nullptr)
    {
      return;
    }

    SourceLocation const loc = func->getLocation();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Skip lambda call operators — trailing return is allowed on lambdas
    if (auto const* rd = dyn_cast<CXXRecordDecl>(func->getParent()))
    {
      if (rd->isLambda())
      {
        return;
      }
    }

    // Skip deduction guides (CTAD)
    if (isa<CXXDeductionGuideDecl>(func))
    {
      return;
    }

    // Skip if already has `auto` return type with no trailing return
    // (hasTrailingReturnType already filtered this, so if we get here
    // it's a non-lambda function with trailing return)

    diag(loc,
         "non-lambda function '%0' uses trailing return type; "
         "use traditional return type syntax")
      << func->getName();
  }
} // namespace clang::tidy::readability
