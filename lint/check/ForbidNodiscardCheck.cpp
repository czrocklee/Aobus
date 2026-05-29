// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ForbidNodiscardCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/AttrKinds.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void ForbidNodiscardCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(functionDecl(hasAttr(attr::WarnUnusedResult)).bind("func"), this);

    finder->addMatcher(cxxRecordDecl(isDefinition(), hasAttr(attr::WarnUnusedResult)).bind("record"), this);
  }

  void ForbidNodiscardCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    if (auto const* func = result.Nodes.getNodeAs<FunctionDecl>("func"); func != nullptr)
    {
      SourceLocation const loc = func->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      diag(loc,
           "remove [[nodiscard]] from '%0'; rely on clang-tidy "
           "unused-return diagnostics instead")
        << func;
    }

    if (auto const* record = result.Nodes.getNodeAs<CXXRecordDecl>("record"); record != nullptr)
    {
      SourceLocation const loc = record->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      diag(loc,
           "remove [[nodiscard]] from '%0'; rely on clang-tidy "
           "unused-return diagnostics instead")
        << record;
    }
  }
} // namespace clang::tidy::readability
