#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/LangOptions.h>

namespace clang::tidy::modernize
{
  class UseRangesAnyOfCheck : public ClangTidyCheck
  {
  public:
    UseRangesAnyOfCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    bool isLanguageVersionSupported(LangOptions const& langOpts) const override { return langOpts.CPlusPlus20; }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::modernize
