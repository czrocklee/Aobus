// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/SpdxLicenseHeaderCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>



using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void SpdxLicenseHeaderCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(translationUnitDecl().bind("tu"), this);
  }

  void SpdxLicenseHeaderCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* tu = result.Nodes.getNodeAs<TranslationUnitDecl>("tu");

    if (tu == nullptr)
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const fid = sm.getMainFileID();
    auto const buf = sm.getBufferData(fid);

    if (buf.empty())
    {
      return;
    }

    auto const firstNewline = buf.find('\n');
    auto const firstLine = buf.substr(0, firstNewline);

    if (!firstLine.starts_with("// SPDX-License-Identifier: MIT"))
    {
      auto const loc = sm.getLocForStartOfFile(fid);

      diag(loc, "file must start with '// SPDX-License-Identifier: MIT'");
    }
  }
} // namespace clang::tidy::readability
