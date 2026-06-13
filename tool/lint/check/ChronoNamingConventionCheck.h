// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /**
   * @brief Enforces semantic naming for std::chrono types.
   *
   * std::chrono::duration declarations must end with a span noun (a measured
   * amount of time), while std::chrono::time_point declarations must end with a
   * point noun (a specific instant). The two vocabularies are kept deliberately
   * disjoint so that a span is never named like an instant or vice versa.
   *
   * Names that stack two time nouns (e.g. "elapsedDuration", "nowTime") are also
   * rejected: the stem already names a complete time concept, so the trailing
   * generic suffix is redundant. Positional words (start, end, ...) are exempt,
   * so idiomatic names like "startTime" remain valid.
   */
  class ChronoNamingConventionCheck : public ClangTidyCheck
  {
  public:
    ChronoNamingConventionCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
