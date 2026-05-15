#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 3.4.5: non-primitive locals use auto x = Type{args},
/// primitives use T x = val (no braces). Diagnostic-only.
class LocalInitializationStyleCheck : public ClangTidyCheck {
public:
  LocalInitializationStyleCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
