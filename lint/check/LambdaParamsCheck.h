#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Checks for lambdas with an empty parameter list and enforces omitting the
/// empty parentheses.
///
/// Rule 3.4.7: Omit the empty parameter list '()' in lambdas that take no
/// arguments (e.g., '[] { ... }' instead of '[]() { ... }').
class LambdaParamsCheck : public ClangTidyCheck {
public:
  LambdaParamsCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
