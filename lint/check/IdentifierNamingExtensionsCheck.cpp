// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/IdentifierNamingExtensionsCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/CharInfo.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/Specifiers.h>
#include <llvm/ADT/StringRef.h>

#include <algorithm>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isCamelBack(StringRef name)
    {
      return !name.empty() && isLowercase(static_cast<unsigned char>(name[0])) && !name.contains('_');
    }

    bool hasPrivateMembers(CXXRecordDecl const* record)
    {
      return std::ranges::contains(record->fields(), AS_private, &Decl::getAccess);
    }
  } // namespace

  void IdentifierNamingExtensionsCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(fieldDecl(hasParent(cxxRecordDecl(isDefinition()).bind("record"))).bind("field"), this);
  }

  void IdentifierNamingExtensionsCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* field = result.Nodes.getNodeAs<FieldDecl>("field");
    auto const* record = result.Nodes.getNodeAs<CXXRecordDecl>("record");

    if ((field == nullptr) || (record == nullptr))
    {
      return;
    }

    if (sm.isInSystemHeader(field->getLocation()) || field->getLocation().isMacroID())
    {
      return;
    }

    StringRef const name = field->getName();

    if (name.empty() || name == "_")
    {
      return;
    }

    if (record->isClass())
    {
      // Class members must be _camelCase
      if (name.starts_with("_"))
      {
        // Check the part after underscore is camelBack
        if (StringRef const rest = name.substr(1); !rest.empty() && !isCamelBack(rest))
        {
          diag(field->getLocation(), "class data member '%0' should be _camelCase after the underscore") << name;
        }
      }
      else
      {
        diag(field->getLocation(), "class data member '%0' must use _camelCase (underscore prefix)") << name;
      }
    }
    else
    {
      // Struct members use camelCase — no underscore prefix.
      // Skip structs with private members (may be class-like).
      if (hasPrivateMembers(record))
      {
        return;
      }

      if (name.starts_with("_"))
      {
        diag(field->getLocation(), "struct data member '%0' should use camelCase without underscore prefix") << name;
      }
    }
  }
} // namespace clang::tidy::readability
