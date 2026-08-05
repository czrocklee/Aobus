// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ResultNamingConventionCheck.h"

#include <clang/AST/Decl.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isConventionalResultName(StringRef name)
    {
      return name == "res" || name == "result" || name == "_result" || name.ends_with("Res");
    }
  } // namespace

  void ResultNamingConventionCheck::registerMatchers(MatchFinder* finder)
  {
    // Robustly match ao::Result by desugaring through TemplateSpecializationType
    // (and deduced auto types); qualType(hasDeclaration(...)) alone cannot see
    // through template specializations.
    finder->addMatcher(declaratorDecl(hasType(qualType(hasUnqualifiedDesugaredType(recordType(
                                        hasDeclaration(classTemplateSpecializationDecl(hasName("::ao::Result"))))))))
                         .bind("result_decl"),
                       this);
  }

  void ResultNamingConventionCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* resultDecl = result.Nodes.getNodeAs<DeclaratorDecl>("result_decl");

    if (resultDecl == nullptr)
    {
      return;
    }

    if (sm.isInSystemHeader(resultDecl->getLocation()) || resultDecl->getLocation().isMacroID())
    {
      return;
    }

    StringRef const name = resultDecl->getName();

    if (name.empty() || name == "_")
    {
      return;
    }

    if (!isConventionalResultName(name))
    {
      auto const* kindStr = "variable";

      if (llvm::isa<FieldDecl>(resultDecl))
      {
        kindStr = "member";
      }
      else if (llvm::isa<ParmVarDecl>(resultDecl))
      {
        kindStr = "parameter";
      }

      auto const kind = StringRef{kindStr};
      diag(resultDecl->getLocation(), "ao::Result %0 '%1' should end with 'Res'") << kind << name;
    }
  }
} // namespace clang::tidy::readability
