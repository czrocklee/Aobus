// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/MemberOrderCheck.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>

#include <cstdint>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    char const* accessName(AccessSpecifier as)
    {
      switch (as)
      {
        case AS_public: return "public";
        case AS_protected: return "protected";
        case AS_private: return "private";
        default: return "";
      }
    }

    bool hasImplicitMembersBeforeAccessSpecifier(CXXRecordDecl const* record)
    {
      for (auto const* decl : record->decls())
      {
        if (decl->isImplicit() || decl == record)
        {
          continue;
        }

        if (isa<AccessSpecDecl>(decl))
        {
          break;
        }

        if (!decl->isImplicit() && !isa<AccessSpecDecl>(decl))
        {
          return true;
        }
      }

      return false;
    }

    void verifyAccessSpecifierOrder(CXXRecordDecl const* record,
                                    std::int32_t lastAccessValue,
                                    bool sawExplicitAccess,
                                    ClangTidyCheck& check)
    {
      bool reported = false;

      for (auto const* decl : record->decls())
      {
        if (decl->isImplicit() || decl == record)
        {
          continue;
        }

        if (SourceLocation const dloc = decl->getLocation(); dloc.isInvalid() || dloc.isMacroID())
        {
          continue;
        }

        if (auto const* as = dyn_cast<AccessSpecDecl>(decl); as != nullptr)
        {
          std::int32_t const newAccess = static_cast<std::int32_t>(as->getAccess());

          if (sawExplicitAccess && newAccess < lastAccessValue && !reported)
          {
            check.diag(as->getLocation(),
                       "'%0' access section appears after '%1'; "
                       "expected public before protected before private (Rule 2.5.2)")
              << accessName(as->getAccess()) << accessName(static_cast<AccessSpecifier>(lastAccessValue));
            reported = true;
          }

          if (!sawExplicitAccess || newAccess > lastAccessValue)
          {
            lastAccessValue = newAccess;
          }

          sawExplicitAccess = true;
        }
      }
    }
  } // namespace

  void MemberOrderCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      cxxRecordDecl(isDefinition(), unless(isExpansionInSystemHeader()), unless(isImplicit())).bind("record"), this);
  }

  void MemberOrderCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* record = result.Nodes.getNodeAs<CXXRecordDecl>("record");

    if (record == nullptr)
    {
      return;
    }

    if (SourceLocation const loc = record->getLocation();
        loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Skip truly empty records (no fields, no methods, no bases)
    if (record->field_empty() && (record->method_begin() == record->method_end()) && record->getNumBases() == 0)
    {
      return;
    }

    // class: default is private (2), struct: default is public (0)
    std::int32_t const lastAccessValue = (record->isClass() && !record->isStruct()) ? 2 : 0;
    bool const sawMemberInImplicit = hasImplicitMembersBeforeAccessSpecifier(record);

    verifyAccessSpecifierOrder(record, lastAccessValue, sawMemberInImplicit, *this);
  }
} // namespace clang::tidy::readability
