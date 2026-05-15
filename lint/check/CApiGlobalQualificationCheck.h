#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Rule 2.6.3: external C library functions and types use ::
/// qualification (e.g. ::mdb_cursor_open(), ::pw_stream_new()).
class CApiGlobalQualificationCheck : public ClangTidyCheck {
public:
  CApiGlobalQualificationCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
