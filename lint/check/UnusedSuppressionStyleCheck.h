// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces Rules 3.2.8 / 3.2.8.1 / 3.2.8.2 / 3.2.8.3:
  /// - No void casts to suppress unused warnings (use [[maybe_unused]] or
  ///   anonymous parameters instead).
  /// - Void casts that discard a computed value must use 'std::ignore = expr'.
  /// - 'std::ignore = var' on a plain variable read is the inverse misuse;
  ///   the declaration should carry [[maybe_unused]] instead.
  class UnusedSuppressionStyleCheck : public ClangTidyCheck
  {
  public:
    UnusedSuppressionStyleCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
