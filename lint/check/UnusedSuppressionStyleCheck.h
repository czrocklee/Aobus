#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rules 3.2.8 / 3.2.8.1 / 3.2.8.2:
/// - No void casts to suppress unused warnings (use [[maybe_unused]] or
///   anonymous parameters instead).
class UnusedSuppressionStyleCheck : public ClangTidyCheck {
public:
  UnusedSuppressionStyleCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
