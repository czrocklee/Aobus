#include "check/RedundantNamespaceQualificationCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/Expr.h>
#include <clang/AST/NestedNameSpecifier.h>
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

using namespace clang::ast_matchers;

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
        if (auto const* decl = parent.get<Decl>())
        {
          if (auto const* dc = dyn_cast<DeclContext>(decl))
          {
            return dc;
          }

          return decl->getDeclContext();
        }

        if (auto const* dc = getEnclosingContext(context, parent))
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
  } // namespace

  void RedundantNamespaceQualificationCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(declRefExpr().bind("declRef"), this);
    finder->addMatcher(
      typeLoc(anyOf(elaboratedTypeLoc().bind("elaboratedTypeLoc"),
                    qualifiedTypeLoc(hasUnqualifiedLoc(elaboratedTypeLoc().bind("elaboratedTypeLoc")))))
        .bind("typeLoc"),
      this);
  }

  void RedundantNamespaceQualificationCheck::check(MatchFinder::MatchResult const& result)
  {
    // Enforces Rule 2.6.5: avoid redundant namespace qualification.
    auto const& sm = *result.SourceManager;
    auto specLoc = NestedNameSpecifierLoc{};
    auto node = DynTypedNode{};

    if (auto const* declRef = result.Nodes.getNodeAs<DeclRefExpr>("declRef"))
    {
      if (!declRef->hasQualifier())
      {
        return;
      }

      specLoc = declRef->getQualifierLoc();
      node = DynTypedNode::create(*declRef);
    }
    else if (auto const* typeLoc = result.Nodes.getNodeAs<ElaboratedTypeLoc>("elaboratedTypeLoc"))
    {
      auto const* contextTypeLoc = result.Nodes.getNodeAs<TypeLoc>("typeLoc");

      if (contextTypeLoc == nullptr)
      {
        return;
      }

      specLoc = typeLoc->getQualifierLoc();
      node = DynTypedNode::create(*contextTypeLoc);
    }
    else
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

    for (auto currentLoc = specLoc; currentLoc; currentLoc = currentLoc.getPrefix())
    {
      chain.push_back(currentLoc);
    }

    std::ranges::reverse(chain);

    auto lastRedundant = NestedNameSpecifierLoc{};

    for (auto const& specLocItem : chain)
    {
      NestedNameSpecifier const* spec = specLocItem.getNestedNameSpecifier();

      if (spec->getKind() == NestedNameSpecifier::Global)
      {
        // Keep global qualification if present, as it's usually intentional.
        break;
      }

      if (spec->getKind() == NestedNameSpecifier::Namespace || spec->getKind() == NestedNameSpecifier::NamespaceAlias)
      {
        NamespaceDecl const* ns = nullptr;

        if (spec->getKind() == NestedNameSpecifier::Namespace)
        {
          ns = spec->getAsNamespace();
        }
        else
        {
          ns = spec->getAsNamespaceAlias()->getNamespace();
        }

        if (ns != nullptr && isAncestor(ns, currentContext))
        {
          lastRedundant = specLocItem;
          continue;
        }
      }

      break;
    }

    if (lastRedundant)
    {
      auto const range = SourceRange{specLoc.getBeginLoc(), lastRedundant.getEndLoc()};
      auto const charRange = CharSourceRange::getCharRange(
        range.getBegin(), Lexer::getLocForEndOfToken(range.getEnd(), 0, sm, result.Context->getLangOpts()));
      auto const redundantText = Lexer::getSourceText(charRange, sm, result.Context->getLangOpts());

      diag(loc, "redundant namespace qualification '%0'") << redundantText << FixItHint::CreateRemoval(range);
    }
  }
} // namespace clang::tidy::readability
