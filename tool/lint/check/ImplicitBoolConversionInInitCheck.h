// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::aobus
{
  /// Finds implicit boolean conversions in if statements with an initialization statement
  /// where the condition is just the initialized variable.
  ///
  /// For example:
  /// \code
  ///   if (auto ptr = raw(); ptr) {} // triggers warning and suggests ptr != nullptr
  /// \endcode
  class ImplicitBoolConversionInInitCheck : public ClangTidyCheck
  {
  public:
    ImplicitBoolConversionInInitCheck(StringRef name, ClangTidyContext* context);
    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::aobus
