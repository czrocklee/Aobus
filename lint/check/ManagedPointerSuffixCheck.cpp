// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ManagedPointerSuffixCheck.h"

#include <clang/AST/Decl.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void ManagedPointerSuffixCheck::registerMatchers(MatchFinder* finder)
  {
    auto const managedPointerType =
      hasType(qualType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(
        cxxRecordDecl(hasAnyName("::std::shared_ptr", "::std::unique_ptr", "::std::weak_ptr", "::Glib::RefPtr")))))));

    finder->addMatcher(declaratorDecl(managedPointerType, unless(matchesName(".*Ptr$"))).bind("root"), this);
  }

  void ManagedPointerSuffixCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* declNode = result.Nodes.getNodeAs<DeclaratorDecl>("root");

    if (declNode == nullptr || declNode->getIdentifier() == nullptr || declNode->getName().empty())
    {
      return;
    }

    diag(declNode->getLocation(), "managed pointer variable '%0' should end with 'Ptr'")
      << declNode->getName();
  }

} // namespace clang::tidy::readability
