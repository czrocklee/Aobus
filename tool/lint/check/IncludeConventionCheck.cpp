// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/IncludeConventionCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
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

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    bool isGeneratedDispatchHeader(StringRef fileName)
    {
      return fileName == "media/file/flac/VorbisCommentDispatch.h" || fileName == "media/file/mp4/AtomDispatch.h" ||
             fileName == "media/file/mpeg/id3v2/FrameDispatch.h";
    }

    class IncludeConventionPPCallbacks final : public PPCallbacks
    {
    public:
      explicit IncludeConventionPPCallbacks(IncludeConventionCheck& check, SourceManager const& sm)
        : _check{check}, _sm{sm}
      {
      }

      void InclusionDirective(SourceLocation hashLoc,
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
        _check.checkInclude(hashLoc, isAngled, fileName, filenameRange, fileType, _sm);
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

  void IncludeConventionCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(decl(isExpansionInMainFile()).bind("decl"), this);
  }

  void IncludeConventionCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* decl = result.Nodes.getNodeAs<Decl>("decl");

    if (decl == nullptr || isa<TranslationUnitDecl>(decl) || isa<LinkageSpecDecl>(decl) || decl->isImplicit())
    {
      return;
    }

    SourceManager const& sm = *result.SourceManager;
    _sm = &sm;

    SourceLocation const loc = sm.getExpansionLoc(decl->getLocation());

    if (loc.isInvalid() || !sm.isWrittenInMainFile(loc))
    {
      return;
    }

    if (_firstMainFileDeclarationLoc.isInvalid() || sm.isBeforeInTranslationUnit(loc, _firstMainFileDeclarationLoc))
    {
      _firstMainFileDeclarationLoc = loc;
    }
  }

  void IncludeConventionCheck::onEndOfTranslationUnit()
  {
    if (_sm != nullptr && _firstMainFileDeclarationLoc.isValid())
    {
      for (SourceLocation const includeLoc : _mainFileIncludeLocs)
      {
        if (_sm->isBeforeInTranslationUnit(_firstMainFileDeclarationLoc, includeLoc))
        {
          diag(includeLoc, "#include directive appears after a C++ declaration; keep all includes before declarations");
        }
      }
    }

    _mainFileIncludeLocs.clear();
    _firstMainFileDeclarationLoc = SourceLocation{};
    _sm = nullptr;
  }

  void IncludeConventionCheck::checkInclude(SourceLocation hashLoc,
                                            bool isAngled,
                                            StringRef fileName,
                                            CharSourceRange filenameRange,
                                            SrcMgr::CharacteristicKind fileType,
                                            SourceManager const& sm)
  {
    SourceLocation const includeLoc = sm.getExpansionLoc(hashLoc);

    if (includeLoc.isValid() && sm.isWrittenInMainFile(includeLoc) && !isGeneratedDispatchHeader(fileName))
    {
      _mainFileIncludeLocs.push_back(includeLoc);
    }

    if (filenameRange.isInvalid())
    {
      return;
    }

    auto const loc = filenameRange.getBegin();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Inline buffer for the angle-bracket-wrapped filename in the fix-it replacement.
    constexpr unsigned kFilenameBufferSize = 128;

    if (isAngled)
    {
      return;
    }

    if (fileType == SrcMgr::C_System || fileType == SrcMgr::C_ExternCSystem)
    {
      auto buf = SmallString<kFilenameBufferSize>{};
      buf.push_back('<');
      buf.append(fileName);
      buf.push_back('>');

      diag(loc, "system include '%0' should use angle brackets")
        << fileName << FixItHint::CreateReplacement(filenameRange, buf.str());
    }
    else if (fileName.starts_with("ao/"))
    {
      auto buf = SmallString<kFilenameBufferSize>{};
      buf.push_back('<');
      buf.append(fileName);
      buf.push_back('>');

      diag(loc, "project include '%0' should use angle brackets")
        << fileName << FixItHint::CreateReplacement(filenameRange, buf.str());
    }
  }
} // namespace clang::tidy::readability
