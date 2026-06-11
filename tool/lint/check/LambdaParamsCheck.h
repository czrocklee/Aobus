// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Checks for lambdas with an empty parameter list and enforces omitting the
  /// empty parentheses.
  ///
  /// Rule 3.4.7: Omit the empty parameter list '()' in lambdas that take no
  /// arguments (e.g., '[] { ... }' instead of '[]() { ... }').
  class LambdaParamsCheck : public ClangTidyCheck
  {
  public:
    LambdaParamsCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
