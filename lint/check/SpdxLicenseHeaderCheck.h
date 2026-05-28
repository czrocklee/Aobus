// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability
{
  class SpdxLicenseHeaderCheck : public ClangTidyCheck
  {
  public:
    SpdxLicenseHeaderCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
