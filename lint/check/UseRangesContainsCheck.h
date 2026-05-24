#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

#include <string>

namespace clang::tidy::readability
{
  class UseRangesContainsCheck : public ClangTidyCheck
  {
  public:
    UseRangesContainsCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;

  private:
    void handleFindReplacement(ast_matchers::MatchFinder::MatchResult const& result,
                               std::string const& replacementBase,
                               bool isFind);

    void handleCountReplacement(ast_matchers::MatchFinder::MatchResult const& result,
                                std::string const& replacementBase,
                                bool isCount);
  };
} // namespace clang::tidy::readability
