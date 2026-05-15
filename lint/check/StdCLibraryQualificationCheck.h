#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 2.6.4: C standard library functions and types available in
/// C++ use std:: qualification (e.g. std::memcpy(), std::size_t).
class StdCLibraryQualificationCheck : public ClangTidyCheck {
public:
  StdCLibraryQualificationCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
