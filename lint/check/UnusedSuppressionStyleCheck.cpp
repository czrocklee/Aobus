// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UnusedSuppressionStyleCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attrs.inc>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void UnusedSuppressionStyleCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(cStyleCastExpr().bind("cast"), this);

    finder->addMatcher(cxxStaticCastExpr().bind("cast"), this);
  }

  void UnusedSuppressionStyleCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* cast = result.Nodes.getNodeAs<ExplicitCastExpr>("cast");

    if (cast == nullptr)
    {
      return;
    }

    SourceLocation const loc = cast->getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Only flag casts to void
    if (!cast->getType()->isVoidType())
    {
      return;
    }

    // Get the sub-expression being cast to void
    auto const* subExpr = cast->getSubExpr();

    if (subExpr == nullptr)
    {
      return;
    }

    // Skip if the sub-expression is already [[maybe_unused]] — the cast is
    // redundant alongside the attribute, but the attribute is the correct fix.
    if (auto const* declRef = dyn_cast<DeclRefExpr>(subExpr->IgnoreParenImpCasts()); declRef != nullptr)
    {
      if (auto const* vd = dyn_cast<VarDecl>(declRef->getDecl()); vd != nullptr)
      {
        if (vd->hasAttr<UnusedAttr>())
        {
          return;
        }
      }
    }

    bool const isCStyle = isa<CStyleCastExpr>(cast);
    diag(loc,
         "void cast %0 suppresses unused warnings; use [[maybe_unused]] on "
         "the declaration or an anonymous parameter instead")
      << (isCStyle ? "'(void)expr'" : "'static_cast<void>(expr)'");
  }
} // namespace clang::tidy::readability
