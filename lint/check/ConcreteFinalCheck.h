#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 4.2.1: concrete classes not designed for inheritance should
/// be marked `final`.  Starts conservative — anonymous-namespace and private
/// nested records only.
class ConcreteFinalCheck : public ClangTidyCheck {
public:
  ConcreteFinalCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
