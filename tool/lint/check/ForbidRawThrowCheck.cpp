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
    // A raw throw is allowed only inside a dedicated throwing helper in the ao
    // namespace tree: a function whose name begins with "throw" followed by an
    // upper-case letter. This admits the core ao::throwException and each
    // subsystem's throw<Domain>Error helper (see doc/spec/failure/outcome-channel.md), but
    // not incidental names such as "throwaway" nor any throw-named helper outside
    // ao. Every other site must raise through such a helper. The "::ao" name pins
    // the top-level namespace, so a foreign "other::ao" would not qualify.
    auto isThrowHelper =
      functionDecl(matchesName("(^|::)throw[A-Z][A-Za-z0-9_]*$"), hasAncestor(namespaceDecl(hasName("::ao"))));

    // std::/boost system_error model a system-call failure at a library boundary
    // that is translated to Result there, so they are exempt from the helper rule.
    auto isAllowedException = hasType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(cxxRecordDecl(hasAnyName("::boost::system::system_error", "::std::system_error"))))));

    finder->addMatcher(
      cxxThrowExpr(unless(hasAncestor(isThrowHelper)), unless(has(expr(isAllowedException)))).bind("throw"), this);
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

    diag(loc,
         "do not use raw 'throw' expression; raise it through a throwing helper such as 'ao::throwException' or a "
         "subsystem 'throw<Domain>Error' helper");
  }
} // namespace clang::tidy::readability
