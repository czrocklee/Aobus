// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces that raw C++ 'throw' expressions are forbidden in general code,
  /// requiring that they be raised through a throwing helper instead: the core
  /// ao::throwException or a subsystem throw<Domain>Error helper. A throwing
  /// helper is a function in the ao namespace tree whose name begins with "throw"
  /// followed by an upper-case letter.
  class ForbidRawThrowCheck : public ClangTidyCheck
  {
  public:
    ForbidRawThrowCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
