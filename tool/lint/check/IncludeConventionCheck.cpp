// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/IncludeConventionCheck.h"

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileEntry.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/Module.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/StringRef.h>

#include <memory>

namespace clang::tidy::readability
{
  namespace
  {
    class IncludeConventionPPCallbacks final : public PPCallbacks
    {
    public:
      explicit IncludeConventionPPCallbacks(IncludeConventionCheck& check, SourceManager const& sm)
        : _check{check}, _sm{sm}
      {
      }

      void InclusionDirective(SourceLocation /*hashLoc*/,
                              Token const& /*includeTok*/,
                              StringRef fileName,
                              bool isAngled,
                              CharSourceRange filenameRange,
                              OptionalFileEntryRef /*file*/,
                              StringRef /*searchPath*/,
                              StringRef /*relativePath*/,
                              Module const* /*suggestedModule*/,
                              bool /*moduleImported*/,
                              SrcMgr::CharacteristicKind fileType) override
      {
        _check.checkInclude(isAngled, fileName, filenameRange, fileType, _sm);
      }

    private:
      IncludeConventionCheck& _check;
      SourceManager const& _sm;
    };
  } // namespace

  void IncludeConventionCheck::registerPPCallbacks(SourceManager const& sm,
                                                   Preprocessor* pp,
                                                   Preprocessor* /*moduleExpanderPP*/)
  {
    pp->addPPCallbacks(std::make_unique<IncludeConventionPPCallbacks>(*this, sm));
  }

  void IncludeConventionCheck::checkInclude(bool isAngled,
                                            StringRef fileName,
                                            CharSourceRange filenameRange,
                                            SrcMgr::CharacteristicKind fileType,
                                            SourceManager const& sm)
  {
    if (filenameRange.isInvalid())
    {
      return;
    }

    auto const loc = filenameRange.getBegin();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    if (isAngled)
    {
      return;
    }

    if (fileType == SrcMgr::C_System || fileType == SrcMgr::C_ExternCSystem)
    {
      auto buf = SmallString<128>{};
      buf.push_back('<');
      buf.append(fileName);
      buf.push_back('>');

      diag(loc, "system include '%0' should use angle brackets")
        << fileName << FixItHint::CreateReplacement(filenameRange, buf.str());
    }
    else if (fileName.starts_with("ao/"))
    {
      auto buf = SmallString<128>{};
      buf.push_back('<');
      buf.append(fileName);
      buf.push_back('>');

      diag(loc, "project include '%0' should use angle brackets")
        << fileName << FixItHint::CreateReplacement(filenameRange, buf.str());
    }
  }
} // namespace clang::tidy::readability
