#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 3.4.6: non-lambda functions must use traditional return type
/// syntax, not trailing return type.
class ForbidTrailingReturnCheck : public ClangTidyCheck {
public:
  ForbidTrailingReturnCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
