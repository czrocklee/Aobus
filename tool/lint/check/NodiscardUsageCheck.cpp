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
#include <clang/Lex/Lexer.h>
#include <llvm/Support/Casting.h>

#include <cstdint>

using namespace clang::ast_matchers;

namespace clang::tidy::modernize
{
  namespace
  {
    CharSourceRange getAttributeRemovalRange(Attr const* attr, SourceManager const& sm, LangOptions const& langOpts)
    {
      SourceRange const range = attr->getRange();
      SourceLocation const beginLoc = range.getBegin();
      SourceLocation const endLoc = range.getEnd();
      SourceLocation const tokenEndLoc = Lexer::getLocForEndOfToken(endLoc, 0, sm, langOpts);

      bool invalid = false;
      char const* beginData = sm.getCharacterData(beginLoc, &invalid);

      if (invalid || beginData == nullptr)
      {
        return CharSourceRange::getTokenRange(range);
      }

      char const* endData = sm.getCharacterData(tokenEndLoc, &invalid);

      if (invalid || endData == nullptr)
      {
        return CharSourceRange::getTokenRange(range);
      }

      // 1. Check if the attribute is the only one in double brackets: [[nodiscard]]
      if (unsigned const offset = sm.getFileOffset(beginLoc);
          offset >= 2 && beginData[-1] == '[' && beginData[-2] == '[' && endData[0] == ']' && endData[1] == ']')
      {
        SourceLocation const fullBegin = beginLoc.getLocWithOffset(-2);
        SourceLocation const fullEnd = tokenEndLoc.getLocWithOffset(2);

        return CharSourceRange::getCharRange(fullBegin, fullEnd);
      }

      // 2. Check if followed by a comma (e.g. [[nodiscard, deprecated]])
      if (endData[0] == ',')
      {
        char const* curr = endData + 1;

        while (*curr == ' ' || *curr == '\t')
        {
          curr++;
        }

        // Reaching the NUL terminator means the attribute list is truncated at
        // EOF; fall back to removing just the attribute token.
        if (*curr == '\0')
        {
          return CharSourceRange::getTokenRange(range);
        }

        SourceLocation const fullEnd = tokenEndLoc.getLocWithOffset(static_cast<std::int32_t>(curr - endData));

        return CharSourceRange::getCharRange(beginLoc, fullEnd);
      }

      // 3. Check if preceded by a comma (e.g. [[deprecated, nodiscard]])
      char const* fileStartData = sm.getCharacterData(sm.getLocForStartOfFile(sm.getFileID(beginLoc)), &invalid);

      if (!invalid && fileStartData != nullptr && beginData > fileStartData)
      {
        char const* curr = beginData - 1;

        while (curr > fileStartData && (*curr == ' ' || *curr == '\t'))
        {
          curr--;
        }

        if (curr >= fileStartData && *curr == ',')
        {
          SourceLocation const fullBegin = beginLoc.getLocWithOffset(static_cast<std::int32_t>(curr - beginData));

          return CharSourceRange::getCharRange(fullBegin, tokenEndLoc);
        }
      }

      return CharSourceRange::getTokenRange(range);
    }
  } // namespace

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

    // Match anything else that HAS [[nodiscard]] but is NOT in our RAII whitelist
    finder->addMatcher(cxxRecordDecl(isDefinition(),
                                     hasAttr(attr::WarnUnusedResult),
                                     unless(anyOf(allOf(isRAII, isWhitelistedRaii), isWhitelistedNodiscard)))
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
          diagnostic << FixItHint::CreateRemoval(getAttributeRemovalRange(nd, sm, result.Context->getLangOpts()));
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
          diagnostic << FixItHint::CreateRemoval(getAttributeRemovalRange(nd, sm, result.Context->getLangOpts()));
        }
      }
    }
  }
} // namespace clang::tidy::modernize
