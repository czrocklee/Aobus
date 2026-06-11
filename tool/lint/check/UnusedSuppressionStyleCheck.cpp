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

    // std::ignore assigned from a pure variable read: the discard idiom is
    // misused as an unused-declaration suppression (Rule 3.2.8.2 territory).
    finder->addMatcher(
      cxxOperatorCallExpr(hasOverloadedOperatorName("="),
                          hasArgument(0, declRefExpr(to(varDecl(hasName("ignore"), isInStdNamespace())))),
                          hasArgument(1, ignoringParenImpCasts(declRefExpr(to(varDecl().bind("readVar"))))))
        .bind("ignoreAssign"),
      this);
  }

  namespace
  {
    // Returns the variable a void cast or std::ignore assignment reads, or
    // nullptr when the discarded operand is not a plain variable read.
    VarDecl const* getSuppressedVar(Expr const* operand)
    {
      if (operand == nullptr)
      {
        return nullptr;
      }

      if (auto const* declRef = dyn_cast<DeclRefExpr>(operand->IgnoreParenImpCasts()); declRef != nullptr)
      {
        return dyn_cast<VarDecl>(declRef->getDecl());
      }

      return nullptr;
    }
  } // namespace

  void UnusedSuppressionStyleCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    if (auto const* ignoreAssign = result.Nodes.getNodeAs<CXXOperatorCallExpr>("ignoreAssign"); ignoreAssign != nullptr)
    {
      SourceLocation const loc = ignoreAssign->getBeginLoc();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      auto const* readVar = result.Nodes.getNodeAs<VarDecl>("readVar");
      diag(loc,
           "'std::ignore = %0' suppresses unused warnings; use [[maybe_unused]] "
           "on the declaration instead and reserve std::ignore for discarding "
           "return values")
        << (readVar != nullptr ? readVar->getName() : StringRef{"expr"});
      return;
    }

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

    bool const isCStyle = isa<CStyleCastExpr>(cast);
    auto const castSpelling = StringRef{isCStyle ? "'(void)expr'" : "'static_cast<void>(expr)'"};

    if (auto const* vd = getSuppressedVar(subExpr); vd != nullptr)
    {
      // Skip if the variable is already [[maybe_unused]] — the cast is
      // redundant alongside the attribute, but the attribute is the correct fix.
      if (vd->hasAttr<UnusedAttr>())
      {
        return;
      }

      diag(loc,
           "void cast %0 suppresses unused warnings; use [[maybe_unused]] on "
           "the declaration or an anonymous parameter instead")
        << castSpelling;
      return;
    }

    // No declaration to attach an attribute to: the cast discards a computed
    // value, which Rule 3.2.8.3 spells as a std::ignore assignment.
    diag(loc, "void cast %0 discards a value; use 'std::ignore = ...' instead") << castSpelling;
  }
} // namespace clang::tidy::readability
