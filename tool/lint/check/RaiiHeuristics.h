// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchersInternal.h>

#include <array>
#include <string_view>

namespace clang::tidy::aobus
{
  namespace detail
  {
    inline constexpr std::string_view kRaiiSuffixPattern =
      "::.*(Guard|Subscription|Registration|Scope|Session|Lock|Transaction|Timer|Writer|Reader|Changes|Tasks|Future|"
      "Handle|"
      "TempDir|TempFile|Token|Raii|Blocker)$";

    inline constexpr auto kRaiiSuffixes = std::to_array<std::string_view>({"Guard",
                                                                           "Subscription",
                                                                           "Registration",
                                                                           "Scope",
                                                                           "Session",
                                                                           "Lock",
                                                                           "Transaction",
                                                                           "Timer",
                                                                           "Writer",
                                                                           "Reader",
                                                                           "Changes",
                                                                           "Tasks",
                                                                           "Future",
                                                                           "Handle",
                                                                           "TempDir",
                                                                           "TempFile",
                                                                           "Token",
                                                                           "Raii",
                                                                           "Blocker"});

    bool isScopedOrRaiiType(QualType type, ASTContext& context);
    bool ownsScopedOrRaiiType(QualType type, ASTContext& context);

    struct IsRAIIMatcher final : public ast_matchers::internal::MatcherInterface<CXXRecordDecl>
    {
      bool matches(CXXRecordDecl const& node,
                   ast_matchers::internal::ASTMatchFinder* finder,
                   ast_matchers::internal::BoundNodesTreeBuilder* builder) const override;
    };
  } // namespace detail

  ast_matchers::internal::Matcher<CXXRecordDecl> isRAII();
  ast_matchers::internal::Matcher<CXXRecordDecl> isWhitelistedRaiiName();
  bool isScopedOrRaiiType(QualType type, ASTContext& context);
} // namespace clang::tidy::aobus
