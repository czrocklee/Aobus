// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyDiagnosticConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

#include <vector>

namespace clang
{
  class Preprocessor;
} // namespace clang

namespace clang::tidy::readability
{
  /// Ensures project-level includes use angle brackets (e.g. <ao/Engine.h>)
  /// while local/private includes keep quotes (e.g. "File.h").
  /// Also flags system/third-party includes that use quotes instead of angle brackets.
  class IncludeConventionCheck : public ClangTidyCheck
  {
  public:
    IncludeConventionCheck(StringRef name, ClangTidyContext* context)
      : ClangTidyCheck{name, context}
    {
    }

    void registerPPCallbacks(SourceManager const& sm, Preprocessor* pp, Preprocessor* moduleExpanderPP) override;
    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(ast_matchers::MatchFinder::MatchResult const& result) override;
    void onEndOfTranslationUnit() override;

    void checkInclude(SourceLocation hashLoc,
                      bool isAngled,
                      StringRef fileName,
                      CharSourceRange filenameRange,
                      SrcMgr::CharacteristicKind fileType,
                      SourceManager const& sm);

  private:
    std::vector<SourceLocation> _mainFileIncludeLocs;
    SourceLocation _firstMainFileDeclarationLoc;
    SourceManager const* _sm = nullptr;
  };
} // namespace clang::tidy::readability
