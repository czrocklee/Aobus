#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability
{
  /// Enforces Rule 2.6.5: avoid redundant namespace qualification when the
  /// reference is already within that namespace (or a sub-namespace).
  class RedundantNamespaceQualificationCheck : public ClangTidyCheck
  {
  public:
    RedundantNamespaceQualificationCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
