#include "check/RedundantNamespaceQualificationCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>

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
        if (auto const* d = parent.get<Decl>())
        {
          if (auto const* dc = dyn_cast<DeclContext>(d))
          {
            return dc;
          }
          return d->getDeclContext();
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
      if ((ancestor == nullptr) || (descendant == nullptr))
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
    finder->addMatcher(typeLoc(loc(elaboratedType())).bind("typeLoc"), this);
  }

  void RedundantNamespaceQualificationCheck::check(MatchFinder::MatchResult const& result)
  {
    // Enforces Rule 2.6.5: avoid redundant namespace qualification.
    auto const& sm = *result.SourceManager;
    NestedNameSpecifierLoc specLoc;
    DynTypedNode node;

    if (auto const* r = result.Nodes.getNodeAs<DeclRefExpr>("declRef"))
    {
      if (!r->hasQualifier())
      {
        return;
      }
      specLoc = r->getQualifierLoc();
      node = DynTypedNode::create(*r);
    }
    else if (auto const* t = result.Nodes.getNodeAs<TypeLoc>("typeLoc"))
    {
      auto et = t->getAs<ElaboratedTypeLoc>();
      if (!et)
      {
        return;
      }
      specLoc = et.getQualifierLoc();
      node = DynTypedNode::create(*t);
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

    SmallVector<NestedNameSpecifierLoc, 4> chain;
    for (NestedNameSpecifierLoc l = specLoc; l; l = l.getPrefix())
    {
      chain.push_back(l);
    }
    std::reverse(chain.begin(), chain.end());

    NestedNameSpecifierLoc lastRedundant;
    for (auto const& l : chain)
    {
      NestedNameSpecifier* spec = l.getNestedNameSpecifier();
      if (spec->getKind() == NestedNameSpecifier::Global)
      {
        // Keep global qualification if present, as it's usually intentional.
        break;
      }

      if ((spec->getKind() == NestedNameSpecifier::Namespace) || (spec->getKind() == NestedNameSpecifier::NamespaceAlias))
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

        if ((ns != nullptr) && isAncestor(ns, currentContext))
        {
          lastRedundant = l;
          continue;
        }
      }
      break;
    }

    if (lastRedundant)
    {
      SourceRange const range{specLoc.getBeginLoc(), lastRedundant.getEndLoc()};
      CharSourceRange const charRange = CharSourceRange::getCharRange(range.getBegin(), Lexer::getLocForEndOfToken(range.getEnd(), 0, sm, result.Context->getLangOpts()));
      StringRef const redundantText = Lexer::getSourceText(charRange, sm, result.Context->getLangOpts());

      diag(loc, "redundant namespace qualification '%0'")
        << redundantText
        << FixItHint::CreateRemoval(range);
    }
  }
} // namespace clang::tidy::readability
