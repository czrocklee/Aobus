// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once
#include <clang-tidy/ClangTidyCheck.h>
#include <clang/Basic/LLVM.h>

namespace clang::ast_matchers
{
  class MatchFinder;
} // namespace clang::ast_matchers

namespace clang::tidy
{
  class ClangTidyContext;
} // namespace clang::tidy

namespace clang::tidy::readability
{
  /// Enforces explicit sized integer types (std::int32_t) over plain C types (int, short).
  /// Excludes types in main() and system headers, and traces external C API usage.
  class UseSizedIntegerTypesCheck : public ClangTidyCheck
  {
  public:
    UseSizedIntegerTypesCheck(StringRef name, ClangTidyContext* context);

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
