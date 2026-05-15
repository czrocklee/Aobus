#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 3.4.3: prefer brace initialization in member initializer
/// lists (e.g. T() : _mem{a} over T() : _mem(a)).
class MemberInitializerBracesCheck : public ClangTidyCheck {
public:
  MemberInitializerBracesCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
