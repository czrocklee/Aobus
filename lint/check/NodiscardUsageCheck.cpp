// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/NodiscardUsageCheck.h"

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
#include <llvm/Support/Casting.h>

using namespace clang::ast_matchers;

namespace clang::tidy::modernize
{
  namespace
  {
    struct IsRAIIMatcher final : public internal::MatcherInterface<CXXRecordDecl>
    {
      bool matches(CXXRecordDecl const& node,
                   internal::ASTMatchFinder* /*finder*/,
                   internal::BoundNodesTreeBuilder* /*builder*/) const override
      {
        bool hasUserProvidedDtor = false;
        bool hasDeletedCopyCtor = false;

        for (auto const* method : node.methods())
        {
          if (auto const* dtor = llvm::dyn_cast<CXXDestructorDecl>(method); dtor != nullptr)
          {
            if (dtor->isUserProvided())
            {
              hasUserProvidedDtor = true;
            }
          }
        }

        for (auto const* ctor : node.ctors())
        {
          if (ctor->isCopyConstructor() && ctor->isDeleted())
          {
            hasDeletedCopyCtor = true;
          }
        }

        return hasUserProvidedDtor && hasDeletedCopyCtor;
      }
    };

    inline internal::Matcher<CXXRecordDecl> isRAII()
    {
      return internal::Matcher{new IsRAIIMatcher{}};
    }
  } // namespace

  void NodiscardUsageCheck::registerMatchers(MatchFinder* finder)
  {
    // Match functions with [[nodiscard]]
    finder->addMatcher(functionDecl(hasAttr(attr::WarnUnusedResult)).bind("func_nodiscard"), this);

    // Define whitelist for classes that MUST be [[nodiscard]] if they are RAII
    auto isWhitelistedRaii = matchesName(
      "::.*(Guard|Subscription|Scope|Session|Lock|Transaction|Timer|Writer|Handle|TempDir|TempFile|Token)$");

    // Match RAII classes that match the whitelist
    finder->addMatcher(cxxRecordDecl(isDefinition(), isRAII(), isWhitelistedRaii).bind("raii_class"), this);

    // Match anything else that HAS [[nodiscard]] but is NOT in our RAII whitelist
    finder->addMatcher(
      cxxRecordDecl(isDefinition(), hasAttr(attr::WarnUnusedResult), unless(allOf(isRAII(), isWhitelistedRaii)))
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

      auto diagnostic = diag(loc, "remove [[nodiscard]] from '%0'; Aobus forbids nodiscard on functions") << func;

      for (auto const* attr : func->attrs())
      {
        if (auto const* nd = llvm::dyn_cast<WarnUnusedResultAttr>(attr); nd != nullptr)
        {
          diagnostic << FixItHint::CreateRemoval(nd->getRange());
        }
      }
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

        diag(loc, "add [[nodiscard]] to RAII class '%0'")
          << raii << FixItHint::CreateInsertion(raii->getLocation(), "[[nodiscard]] ");
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

      auto diagnostic = diag(loc, "remove [[nodiscard]] from non-RAII class '%0'") << nonRaii;

      for (auto const* attr : nonRaii->attrs())
      {
        if (auto const* nd = llvm::dyn_cast<WarnUnusedResultAttr>(attr); nd != nullptr)
        {
          diagnostic << FixItHint::CreateRemoval(nd->getRange());
        }
      }
    }
  }
} // namespace clang::tidy::modernize
