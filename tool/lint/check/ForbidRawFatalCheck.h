// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "check/AstHelpers.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang-tidy/ClangTidyOptions.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Rejects direct references to process-termination primitives outside the
  /// structurally marked Core fatal backend.
  class ForbidRawFatalCheck : public ClangTidyCheck
  {
  public:
    ForbidRawFatalCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}, _sourceScope{*this, Options}
    {
    }

    void storeOptions(ClangTidyOptions::OptionMap& options) override;
    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;

  private:
    aobus::PolicySourceScope const _sourceScope;
  };
} // namespace clang::tidy::readability
