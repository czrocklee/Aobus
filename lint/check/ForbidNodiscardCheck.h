#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces Rule 3.2.7: do not use [[nodiscard]].  Rely on the unused-return
  /// diagnostic instead.
  class ForbidNodiscardCheck : public ClangTidyCheck
  {
  public:
    ForbidNodiscardCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck(name, context)
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
