#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>

namespace clang::tidy::readability
{
  /// Enforces threading rules 4.4.1-4.4.4:
  /// - Prefer std::jthread over std::thread (4.4.2)
  /// - Prefer std::scoped_lock over std::unique_lock where possible (4.4.3)
  /// - Avoid volatile for shared state (4.4.4)
  /// Warning-only, never auto-fix.
  class ThreadingPolicyCheck : public ClangTidyCheck
  {
  public:
    ThreadingPolicyCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck(name, context)
    {
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
  };
} // namespace clang::tidy::readability
