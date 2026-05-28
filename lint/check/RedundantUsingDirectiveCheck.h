// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

namespace clang::tidy::readability
{
  /**
   * @brief Detects redundant 'using namespace' declarations inside a namespace that is already a child of the nominated
   * namespace.
   */
  class RedundantUsingDirectiveCheck final : public ClangTidyCheck
  {
  public:
    using ClangTidyCheck::ClangTidyCheck;

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
