// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/NodiscardUsageCheck.h"

#include "check/RaiiHeuristics.h"

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchersInternal.h>
#include <clang/Basic/AttrKinds.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

using namespace clang::ast_matchers;

namespace clang::tidy::modernize
{
  void NodiscardUsageCheck::registerMatchers(MatchFinder* finder)
  {
    // Match functions with [[nodiscard]]
    finder->addMatcher(functionDecl(hasAttr(attr::WarnUnusedResult)).bind("func_nodiscard"), this);

    // Use shared RAII logic from aobus namespace
    auto isWhitelistedRaii = aobus::isWhitelistedRaiiName();
    auto isRAII = aobus::isRAII();

    // Match RAII classes that match the whitelist
    finder->addMatcher(cxxRecordDecl(isDefinition(), isRAII, isWhitelistedRaii).bind("raii_class"), this);

    auto isWhitelistedNodiscard = cxxRecordDecl(hasName("::ao::Result"));

    // A structurally RAII class may opt into [[nodiscard]] even when its domain
    // name is outside the suffix whitelist. The whitelist controls which RAII
    // classes must carry the attribute; it is not the set of allowed owners.
    finder->addMatcher(
      cxxRecordDecl(isDefinition(), hasAttr(attr::WarnUnusedResult), unless(anyOf(isRAII, isWhitelistedNodiscard)))
        .bind("non_raii_nodiscard"),
      this);
  }

  void NodiscardUsageCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    // 1. Forbid [[nodiscard]] on functions
    if (auto const* func = result.Nodes.getNodeAs<FunctionDecl>("func_nodiscard"); func != nullptr)
    {
      SourceLocation const loc = func->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      diag(loc, "remove [[nodiscard]] from '%0'; Aobus forbids nodiscard on functions") << func;
    }

    // 2. Require [[nodiscard]] on RAII classes
    if (auto const* raii = result.Nodes.getNodeAs<CXXRecordDecl>("raii_class"); raii != nullptr)
    {
      if (!raii->hasAttr<WarnUnusedResultAttr>())
      {
        SourceLocation const loc = raii->getLocation();

        if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
        {
          return;
        }

        diag(loc, "add [[nodiscard]] to RAII class '%0'") << raii;
      }
    }

    // 3. Forbid [[nodiscard]] on non-RAII classes
    if (auto const* nonRaii = result.Nodes.getNodeAs<CXXRecordDecl>("non_raii_nodiscard"); nonRaii != nullptr)
    {
      SourceLocation const loc = nonRaii->getLocation();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      diag(loc, "remove [[nodiscard]] from non-RAII class '%0'") << nonRaii;
    }
  }
} // namespace clang::tidy::modernize
