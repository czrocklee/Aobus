// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Checks that std::optional variables follow Aobus naming conventions and
  /// usage patterns.
  ///
  /// Rule 1: std::optional variables (locals, members, parameters) must start
  /// with the 'opt' prefix (after any leading underscores for members like
  /// `_opt`).
  /// Rule 2: Existence checks on named optional variables and fields should use
  /// boolean conversion, while temporary optional expressions may use
  /// `.has_value()` when that reads more clearly.
  /// Rule 3: `static_cast<bool>(optional)` is avoided. Use the optional
  /// directly in boolean contexts, and `.has_value()` when materializing a bool.
  class OptionalNamingAndUsageCheck : public ClangTidyCheck
  {
  public:
    OptionalNamingAndUsageCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
