// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces the threading policy:
  /// - Prefer std::jthread over std::thread.
  /// - Prefer std::scoped_lock over std::unique_lock where possible.
  /// - Avoid volatile for shared state.
  /// Warning-only, never auto-fix.
  class ThreadingPolicyCheck : public ClangTidyCheck
  {
  public:
    ThreadingPolicyCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
