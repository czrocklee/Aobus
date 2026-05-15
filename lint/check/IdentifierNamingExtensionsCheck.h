#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Enforces Aobus Rule 2.2.4 member naming beyond what built-in
/// readability-identifier-naming can express:
/// - class non-static data members use _camelCase
/// - struct/POD/Impl/helper members use camelCase (no underscore)
class IdentifierNamingExtensionsCheck : public ClangTidyCheck {
public:
  IdentifierNamingExtensionsCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::readability
