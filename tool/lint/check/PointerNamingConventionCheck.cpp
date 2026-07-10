// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/PointerNamingConventionCheck.h"

#include <clang/AST/Decl.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>

#include <cctype>

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  void PointerNamingConventionCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    auto const managedPointerRecord = recordType(hasDeclaration(
      cxxRecordDecl(hasAnyName("::std::shared_ptr", "::std::unique_ptr", "::std::weak_ptr", "::Glib::RefPtr"))));
    auto const managedPointerObjectType = qualType(hasUnqualifiedDesugaredType(managedPointerRecord));
    auto const managedPointerType =
      hasType(qualType(anyOf(hasUnqualifiedDesugaredType(managedPointerRecord), references(managedPointerObjectType))));

    finder->addMatcher(declaratorDecl(managedPointerType).bind("managed"), this);

    auto const rawPointerObjectType = qualType(hasUnqualifiedDesugaredType(pointerType()));
    auto const rawPointerType =
      hasType(qualType(anyOf(hasUnqualifiedDesugaredType(pointerType()), references(rawPointerObjectType))));
    finder->addMatcher(declaratorDecl(rawPointerType).bind("raw"), this);
  }

  void PointerNamingConventionCheck::check(MatchFinder::MatchResult const& result)
  {
    auto checkHungarian = [this](DeclaratorDecl const* decl, StringRef name)
    {
      if (name.starts_with("p") && name.size() > 1 && std::isupper(static_cast<unsigned char>(name[1])))
      {
        diag(decl->getLocation(), "pointer variable '%0' must not use Hungarian notation ('p' prefix)") << name;
        return true;
      }

      if (name.starts_with("_p") && name.size() > 2 && std::isupper(static_cast<unsigned char>(name[2])))
      {
        diag(decl->getLocation(), "pointer variable '%0' must not use Hungarian notation ('_p' prefix)") << name;
        return true;
      }

      return false;
    };

    if (auto const* managed = result.Nodes.getNodeAs<DeclaratorDecl>("managed");
        managed != nullptr && managed->getIdentifier() != nullptr && !managed->getName().empty())
    {
      auto name = managed->getName();
      checkHungarian(managed, name);

      if (!name.ends_with("Ptr"))
      {
        diag(managed->getLocation(), "managed pointer variable '%0' should end with 'Ptr'") << name;
      }
    }

    if (auto const* raw = result.Nodes.getNodeAs<DeclaratorDecl>("raw");
        raw != nullptr && raw->getIdentifier() != nullptr && !raw->getName().empty())
    {
      auto name = raw->getName();
      checkHungarian(raw, name);

      if (name.ends_with("Ptr"))
      {
        diag(
          raw->getLocation(), "raw pointer variable '%0' must not use the 'Ptr' suffix reserved for managed pointers")
          << name;
      }
    }
  }
} // namespace clang::tidy::readability
