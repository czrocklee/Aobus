// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/RedundantUsingDirectiveCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/Support/Casting.h>

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    bool isSameNamespace(DeclContext const* first, DeclContext const* second)
    {
      if (first == second)
      {
        return true;
      }

      if (first == nullptr || second == nullptr)
      {
        return false;
      }

      if (first->getDeclKind() == Decl::Namespace && second->getDeclKind() == Decl::Namespace)
      {
        return llvm::cast<NamespaceDecl>(first)->getCanonicalDecl() ==
               llvm::cast<NamespaceDecl>(second)->getCanonicalDecl();
      }

      return false;
    }

    bool isAncestor(DeclContext const* ancestor, DeclContext const* descendant)
    {
      if (ancestor == nullptr || descendant == nullptr)
      {
        return false;
      }

      for (auto const* dc = descendant; dc != nullptr; dc = dc->getParent())
      {
        if (isSameNamespace(ancestor, dc))
        {
          return true;
        }
      }

      return false;
    }
  } // namespace

  void RedundantUsingDirectiveCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(usingDirectiveDecl().bind("using"), this);
  }

  void RedundantUsingDirectiveCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* usingDecl = result.Nodes.getNodeAs<UsingDirectiveDecl>("using");

    if (usingDecl == nullptr)
    {
      return;
    }

    SourceLocation const loc = usingDecl->getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || result.SourceManager->isInSystemHeader(loc))
    {
      return;
    }

    NamespaceDecl const* nominated = usingDecl->getNominatedNamespace();
    DeclContext const* currentContext = usingDecl->getDeclContext();

    if (nominated != nullptr && isAncestor(nominated, currentContext))
    {
      diag(loc, "redundant 'using namespace %0' declaration inside its own namespace")
        << nominated->getNameAsString() << FixItHint::CreateRemoval(usingDecl->getSourceRange());
    }
  }
} // namespace clang::tidy::readability
