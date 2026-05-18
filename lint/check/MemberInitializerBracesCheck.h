#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces Rule 3.4.3: prefer brace initialization in member initializer
  /// lists (e.g. T() : _mem{a} over T() : _mem(a)).
  class MemberInitializerBracesCheck : public ClangTidyCheck
  {
  public:
    MemberInitializerBracesCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck(name, context)
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
