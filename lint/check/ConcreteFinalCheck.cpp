// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ConcreteFinalCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attrs.inc>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/CharInfo.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>

#include <algorithm>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isSafeConcreteCandidate(CXXRecordDecl const* record)
    {
      if (record == nullptr)
      {
        return false;
      }

      // Skip if already final (should not happen due to matcher, but safe)
      if (record->hasAttr<FinalAttr>())
      {
        return false;
      }

      // Skip if has virtual member functions
      if (record->isPolymorphic())
      {
        return false;
      }

      // Skip if inherits from another class
      if (record->getNumBases() > 0)
      {
        return false;
      }

      // Skip if the class name suggests interface or base
      auto const* id = record->getIdentifier();

      if (id == nullptr)
      {
        return false;
      }

      StringRef const name = id->getName();

      if (name.starts_with("I") && name.size() > 1 && isUppercase(name[1]))
      {
        return false;
      }

      if (name.ends_with("Base"))
      {
        return false;
      }

      // Skip if has protected ctor or dtor — suggests designed for inheritance
      if (std::ranges::contains(record->ctors(), AS_protected, &Decl::getAccess))
      {
        return false;
      }

      if (auto const* dtor = record->getDestructor())
      {
        if (dtor->getAccess() == AS_protected)
        {
          return false;
        }
      }

      // Skip template specializations
      if (record->getTemplateSpecializationKind() != TSK_Undeclared)
      {
        return false;
      }

      return true;
    }
  } // namespace

  void ConcreteFinalCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      cxxRecordDecl(isDefinition(), unless(isFinal()), unless(isExpansionInSystemHeader())).bind("record"), this);
  }

  void ConcreteFinalCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* record = result.Nodes.getNodeAs<CXXRecordDecl>("record");

    if (record == nullptr)
    {
      return;
    }

    SourceLocation const loc = record->getLocation();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Only check:
    // - types in anonymous namespace
    // - private nested types
    bool inAnonymousNamespace = false;
    bool isPrivateNested = false;

    auto const* dc = record->getDeclContext();

    if (auto const* ns = dyn_cast<NamespaceDecl>(dc))
    {
      if (ns->isAnonymousNamespace())
      {
        inAnonymousNamespace = true;
      }
    }
    else if ((dc != nullptr) && isa<CXXRecordDecl>(dc))
    {
      if (record->getAccess() == AS_private)
      {
        isPrivateNested = true;
      }
    }

    if (!inAnonymousNamespace && !isPrivateNested)
    {
      return;
    }

    if (!isSafeConcreteCandidate(record))
    {
      return;
    }

    auto diagBuilder = diag(loc, "concrete %0 '%1' should be marked 'final'")
                       << (record->isClass() ? "class" : "struct") << record;

    // Provide fix-it: insert 'final' before the opening brace or class body
    if (auto const* classDef = record->getDefinition())
    {
      SourceLocation const braceLoc = classDef->getBraceRange().getBegin();

      if (braceLoc.isValid())
      {
        diagBuilder << FixItHint::CreateInsertion(braceLoc, "final ");
      }
    }
  }
} // namespace clang::tidy::readability
