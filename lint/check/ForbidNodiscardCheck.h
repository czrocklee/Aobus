#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 3.2.7: do not use [[nodiscard]].  Rely on the unused-return
/// diagnostic instead.
class ForbidNodiscardCheck : public ClangTidyCheck {
public:
  ForbidNodiscardCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
