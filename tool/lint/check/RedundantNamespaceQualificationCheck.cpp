// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/RedundantNamespaceQualificationCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclarationName.h>
#include <clang/AST/Expr.h>
#include <clang/AST/NestedNameSpecifier.h> // NOLINT(misc-include-cleaner) -- inline location accessors
#include <clang/AST/NestedNameSpecifierBase.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallVector.h>

#include <algorithm>

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    DeclContext const* getEnclosingContext(ASTContext& context, DynTypedNode const& node)
    {
      auto parents = context.getParents(node);

      if (parents.empty())
      {
        return nullptr;
      }

      for (auto const& parent : parents)
      {
        if (auto const* decl = parent.get<Decl>(); decl != nullptr)
        {
          if (auto const* dc = dyn_cast<DeclContext>(decl); dc != nullptr)
          {
            return dc;
          }

          return decl->getDeclContext();
        }

        if (auto const* dc = getEnclosingContext(context, parent); dc != nullptr)
        {
          return dc;
        }
      }

      return nullptr;
    }

    bool isAncestor(DeclContext const* ancestor, DeclContext const* descendant)
    {
      if (ancestor == nullptr || descendant == nullptr)
      {
        return false;
      }

      for (auto const* dc = descendant; dc != nullptr; dc = dc->getParent())
      {
        if (dc == ancestor)
        {
          return true;
        }
      }

      return false;
    }

    bool isNamespaceRedundant(NestedNameSpecifierLoc const& specLocItem, DeclContext const* currentContext)
    {
      auto const spec = specLocItem.getNestedNameSpecifier();

      if (!spec || spec.getKind() != NestedNameSpecifier::Kind::Namespace)
      {
        return false;
      }

      auto const* namespaceBase = spec.getAsNamespaceAndPrefix().Namespace;
      auto const* ns = namespaceBase != nullptr ? namespaceBase->getNamespace() : nullptr;

      return ns != nullptr && isAncestor(ns, currentContext);
    }

    bool declaresNamespaceNamed(DeclContext const* context, DeclarationName name)
    {
      if (context == nullptr || name.isEmpty())
      {
        return false;
      }

      auto declaresInSingleContext = [name](DeclContext const* candidate)
      {
        return std::ranges::any_of(candidate->decls(),
                                   [name](auto const* decl)
                                   {
                                     if (auto const* nsDecl = dyn_cast<NamespaceDecl>(decl);
                                         nsDecl != nullptr && nsDecl->getDeclName() == name)
                                     {
                                       return true;
                                     }

                                     if (auto const* aliasDecl = dyn_cast<NamespaceAliasDecl>(decl);
                                         aliasDecl != nullptr && aliasDecl->getDeclName() == name)
                                     {
                                       return true;
                                     }

                                     return false;
                                   });
      };

      if (declaresInSingleContext(context))
      {
        return true;
      }

      if (auto const* nsContext = dyn_cast<NamespaceDecl>(context); nsContext != nullptr)
      {
        for (auto const* redecl : nsContext->redecls())
        {
          if (redecl != nsContext && declaresInSingleContext(redecl))
          {
            return true;
          }
        }
      }

      return false;
    }

    bool isShadowedByIntermediateNamespace(NestedNameSpecifierLoc const& lastRedundant,
                                           llvm::SmallVectorImpl<NestedNameSpecifierLoc> const& chain,
                                           DeclContext const* currentContext)
    {
      auto const redundantSpec = lastRedundant.getNestedNameSpecifier();
      bool foundRedundant = false;

      for (auto const& item : chain)
      {
        if (item.getNestedNameSpecifier() == redundantSpec)
        {
          foundRedundant = true;
          continue;
        }

        if (!foundRedundant)
        {
          continue;
        }

        auto const nextSpec = item.getNestedNameSpecifier();

        if (!nextSpec || nextSpec.getKind() != NestedNameSpecifier::Kind::Namespace)
        {
          break;
        }

        auto const* targetNamespaceBase = nextSpec.getAsNamespaceAndPrefix().Namespace;
        auto const* targetNs = targetNamespaceBase != nullptr ? targetNamespaceBase->getNamespace() : nullptr;

        if (targetNs == nullptr)
        {
          break;
        }

        auto const targetName = targetNs->getDeclName();
        auto const* redundantNamespaceBase = redundantSpec.getAsNamespaceAndPrefix().Namespace;
        auto const* const redundantNs =
          redundantNamespaceBase != nullptr ? redundantNamespaceBase->getNamespace() : nullptr;

        for (DeclContext const* dc = currentContext; dc != nullptr && dc != redundantNs; dc = dc->getParent())
        {
          if (auto const* nsDecl = dyn_cast<NamespaceDecl>(dc);
              nsDecl != nullptr && nsDecl->getDeclName() == targetName)
          {
            return true;
          }

          if (declaresNamespaceNamed(dc, targetName))
          {
            return true;
          }
        }

        break;
      }

      return false;
    }

    bool extractSpecLocAndNode(MatchFinder::MatchResult const& result,
                               NestedNameSpecifierLoc& specLoc,
                               DynTypedNode& node)
    {
      if (auto const* declRef = result.Nodes.getNodeAs<DeclRefExpr>("declRef"); declRef != nullptr)
      {
        if (!declRef->hasQualifier())
        {
          return false;
        }

        specLoc = declRef->getQualifierLoc();
        node = DynTypedNode::create(*declRef);
        return true;
      }

      if (auto const* typeLoc = result.Nodes.getNodeAs<TypeLoc>("typeLoc"); typeLoc != nullptr)
      {
        specLoc = typeLoc->getUnqualifiedLoc().getPrefix();

        if (!specLoc)
        {
          return false;
        }

        node = DynTypedNode::create(*typeLoc);
        return true;
      }

      return false;
    }
  } // namespace

  void RedundantNamespaceQualificationCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(declRefExpr().bind("declRef"), this);
    finder->addMatcher(typeLoc().bind("typeLoc"), this);
  }

  void RedundantNamespaceQualificationCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto specLoc = NestedNameSpecifierLoc{};
    auto node = DynTypedNode{};

    if (!extractSpecLocAndNode(result, specLoc, node))
    {
      return;
    }

    if (!specLoc)
    {
      return;
    }

    SourceLocation const loc = specLoc.getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    DeclContext const* currentContext = getEnclosingContext(*result.Context, node);

    if (currentContext == nullptr)
    {
      return;
    }

    auto chain = SmallVector<NestedNameSpecifierLoc, 4>{};

    for (auto currentLoc = specLoc; currentLoc;)
    {
      chain.push_back(currentLoc);

      if (auto const typeLoc = currentLoc.getAsTypeLoc(); !typeLoc.isNull())
      {
        currentLoc = typeLoc.getPrefix();
      }
      else
      {
        currentLoc = currentLoc.getAsNamespaceAndPrefix().Prefix;
      }
    }

    std::ranges::reverse(chain);

    auto lastRedundant = NestedNameSpecifierLoc{};

    for (auto const& specLocItem : chain)
    {
      if (!isNamespaceRedundant(specLocItem, currentContext))
      {
        break;
      }

      lastRedundant = specLocItem;
    }

    if (!lastRedundant)
    {
      return;
    }

    if (isShadowedByIntermediateNamespace(lastRedundant, chain, currentContext))
    {
      return;
    }

    auto const range = SourceRange{specLoc.getBeginLoc(), lastRedundant.getEndLoc()};
    auto const charRange = CharSourceRange::getCharRange(
      range.getBegin(), Lexer::getLocForEndOfToken(range.getEnd(), 0, sm, result.Context->getLangOpts()));
    auto const redundantText = Lexer::getSourceText(charRange, sm, result.Context->getLangOpts());

    if (!redundantText.ends_with("::"))
    {
      return;
    }

    diag(loc, "redundant namespace qualification '%0'") << redundantText << FixItHint::CreateRemoval(range);
  }
} // namespace clang::tidy::readability
