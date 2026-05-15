#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces threading rules 4.4.1-4.4.4:
/// - Prefer std::jthread over std::thread (4.4.2)
/// - Prefer std::scoped_lock over std::unique_lock where possible (4.4.3)
/// - Avoid volatile for shared state (4.4.4)
/// Warning-only, never auto-fix.
class ThreadingPolicyCheck : public ClangTidyCheck {
public:
  ThreadingPolicyCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
