#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Checks that std::optional variables follow Aobus naming conventions and
/// usage patterns.
///
/// Rule 1: std::optional variables (locals, members, parameters) must start
/// with the 'opt' prefix.
/// Rule 2: Existence checks must use 'if (opt)' or 'if (!opt)', not '.has_value()'.
class OptionalNamingAndUsageCheck : public ClangTidyCheck {
public:
  OptionalNamingAndUsageCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
