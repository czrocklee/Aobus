#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces Aobus Rule 2.2.4 member naming beyond what built-in
  /// readability-identifier-naming can express:
  /// - class non-static data members use _camelCase
  /// - struct/POD/Impl/helper members use camelCase (no underscore)
  class IdentifierNamingExtensionsCheck : public ClangTidyCheck
  {
  public:
    IdentifierNamingExtensionsCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
