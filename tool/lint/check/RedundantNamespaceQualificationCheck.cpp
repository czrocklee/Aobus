// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/RedundantNamespaceQualificationCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/DeclarationName.h>
#include <clang/AST/Expr.h>
#include <clang/AST/NestedNameSpecifier.h> // NOLINT(misc-include-cleaner) -- inline location accessors
#include <clang/AST/NestedNameSpecifierBase.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TemplateName.h>
#include <clang/AST/Type.h>
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
#include <cstdint>
#include <optional>

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    struct SourceScopes final
    {
      SmallVector<Decl const*, 4> enclosingDeclarations;
      SmallVector<Decl const*, 8> owners;
    };

    std::optional<SourceScopes> getSourceScopes(ASTContext& context, DynTypedNode const& node)
    {
      struct Frame
      {
        DynTypedNode node;
        bool hasEnclosingDeclaration = false;
        bool leaving = false;
      };

      auto scopes = SourceScopes{};
      auto pending = SmallVector<Frame, 16>{{.node = node}};
      auto active = SmallVector<DynTypedNode, 16>{};
      auto completed = SmallVector<Frame, 16>{};

      while (!pending.empty())
      {
        auto frame = pending.pop_back_val();

        if (frame.leaving)
        {
          active.pop_back();
          completed.push_back(frame);
          continue;
        }

        auto const* declaration = frame.node.get<Decl>();

        // Only source-bearing parent nodes participate in this proof. In
        // particular, canonical Type and QualType nodes do not identify scope.
        if (declaration == nullptr && frame.node.get<Stmt>() == nullptr && frame.node.get<TypeLoc>() == nullptr &&
            frame.node.get<NestedNameSpecifierLoc>() == nullptr && frame.node.get<CXXCtorInitializer>() == nullptr &&
            frame.node.get<Attr>() == nullptr)
        {
          return std::nullopt;
        }

        if (std::ranges::contains(active, frame.node))
        {
          return std::nullopt;
        }

        if (std::ranges::any_of(
              completed,
              [&frame](auto const& prior)
              { return prior.node == frame.node && prior.hasEnclosingDeclaration == frame.hasEnclosingDeclaration; }))
        {
          continue;
        }

        if (declaration != nullptr)
        {
          if (!frame.hasEnclosingDeclaration && !std::ranges::contains(scopes.enclosingDeclarations, declaration))
          {
            scopes.enclosingDeclarations.push_back(declaration);
          }

          // No template can enclose a namespace or the translation unit.
          if (isa<NamespaceDecl>(declaration) || isa<TranslationUnitDecl>(declaration))
          {
            continue;
          }

          if (!std::ranges::contains(scopes.owners, declaration))
          {
            scopes.owners.push_back(declaration);
          }
        }

        auto const parents = context.getParents(frame.node);

        if (parents.empty())
        {
          return std::nullopt;
        }

        active.push_back(frame.node);
        frame.leaving = true;
        pending.push_back(frame);

        for (auto const& parent : parents)
        {
          pending.push_back(
            {.node = parent, .hasEnclosingDeclaration = frame.hasEnclosingDeclaration || declaration != nullptr});
        }
      }

      if (scopes.enclosingDeclarations.empty())
      {
        return std::nullopt;
      }

      return scopes;
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

    NamespaceDecl const* namespaceDecl(NestedNameSpecifierLoc const& specLocItem)
    {
      auto const spec = specLocItem.getNestedNameSpecifier();

      if (!spec || spec.getKind() != NestedNameSpecifier::Kind::Namespace)
      {
        return nullptr;
      }

      auto const* namespaceBase = spec.getAsNamespaceAndPrefix().Namespace;
      return namespaceBase != nullptr ? namespaceBase->getNamespace() : nullptr;
    }

    bool isNamespaceRedundant(NestedNameSpecifierLoc const& specLocItem, DeclContext const* currentContext)
    {
      auto const* ns = namespaceDecl(specLocItem);
      return ns != nullptr && isAncestor(ns, currentContext);
    }

    NamedDecl const* referencedTypeDeclaration(Type const* type)
    {
      // Keep source sugar: an imported alias may have a different lookup name
      // from its canonical record, enum, or template declaration.
      if (auto const* imported = dyn_cast<UsingType>(type); imported != nullptr)
      {
        return imported->getDecl();
      }

      if (auto const* alias = dyn_cast<TypedefType>(type); alias != nullptr)
      {
        return alias->getDecl();
      }

      if (auto const* tag = dyn_cast<TagType>(type); tag != nullptr)
      {
        return tag->getDecl();
      }

      auto name = TemplateName{};

      if (auto const* specialization = dyn_cast<TemplateSpecializationType>(type); specialization != nullptr)
      {
        name = specialization->getTemplateName();
      }
      else if (auto const* deduced = dyn_cast<DeducedTemplateSpecializationType>(type); deduced != nullptr)
      {
        name = deduced->getTemplateName();
      }

      if (auto const* imported = name.getAsUsingShadowDecl(); imported != nullptr)
      {
        return imported;
      }

      return name.getAsTemplateDecl();
    }

    NamedDecl const* firstSurvivingDeclaration(MatchFinder::MatchResult const& result,
                                               NestedNameSpecifierLoc const& lastRedundant,
                                               llvm::SmallVectorImpl<NestedNameSpecifierLoc> const& chain)
    {
      bool foundRedundant = false;

      for (auto const& item : chain)
      {
        if (foundRedundant)
        {
          if (auto const spec = item.getNestedNameSpecifier(); spec.getKind() == NestedNameSpecifier::Kind::Namespace)
          {
            return spec.getAsNamespaceAndPrefix().Namespace;
          }

          auto const typeLoc = item.getAsTypeLoc();
          return typeLoc.isNull() ? nullptr : referencedTypeDeclaration(typeLoc.getTypePtr());
        }

        foundRedundant = item.getNestedNameSpecifier() == lastRedundant.getNestedNameSpecifier();
      }

      if (auto const* declRef = result.Nodes.getNodeAs<DeclRefExpr>("declRef"); declRef != nullptr)
      {
        return declRef->getFoundDecl();
      }

      if (auto const* typeLoc = result.Nodes.getNodeAs<TypeLoc>("typeLoc"); typeLoc != nullptr)
      {
        return referencedTypeDeclaration(typeLoc->getTypePtr());
      }

      return nullptr;
    }

    bool hasSameNamespaceLookup(NamedDecl const* declaration, NestedNameSpecifierLoc const& lastRedundant)
    {
      auto const* ns = namespaceDecl(lastRedundant);

      if (declaration == nullptr || declaration->getDeclName().isEmpty() || ns == nullptr)
      {
        return false;
      }

      auto const lookup = ns->lookup(declaration->getDeclName());
      return std::ranges::any_of(lookup,
                                 [declaration](auto const* candidate)
                                 { return candidate->getCanonicalDecl() == declaration->getCanonicalDecl(); });
    }

    bool hasShadowingTemplateParameter(Decl const* declaration, DeclarationName name)
    {
      auto hasNamedParameter = [name](TemplateParameterList const* parameters)
      {
        return parameters != nullptr &&
               std::ranges::any_of(
                 *parameters, [name](auto const* parameter) { return parameter->getDeclName() == name; });
      };
      auto hasOuterParameter = [&hasNamedParameter](auto const* owner)
      {
        if (owner == nullptr)
        {
          return false;
        }

        for (std::uint32_t index = 0; index < owner->getNumTemplateParameterLists(); ++index)
        {
          if (hasNamedParameter(owner->getTemplateParameterList(index)))
          {
            return true;
          }
        }

        return false;
      };

      auto hasPatternParameter = [declaration, name](auto const* owner)
      {
        if (owner == nullptr)
        {
          return false;
        }

        auto const* pattern = owner->getTemplateInstantiationPattern();

        if (pattern != nullptr)
        {
          if (auto const* definition = pattern->getDefinition(); definition != nullptr)
          {
            pattern = definition;
          }
        }

        return pattern != nullptr && pattern != declaration && hasShadowingTemplateParameter(pattern, name);
      };

      auto const* templateDeclaration = dyn_cast<TemplateDecl>(declaration);

      // Concepts and leading requires-clauses are owned by TemplateDecl,
      // rather than the templated declaration or another DeclContext.
      // Out-of-line definitions can rename an enclosing class template's
      // parameters. Instantiated nodes share their source locations but may
      // omit those outer lists, so also inspect the source definition pattern.
      return (templateDeclaration != nullptr && hasNamedParameter(templateDeclaration->getTemplateParameters())) ||
             hasNamedParameter(declaration->getDescribedTemplateParams()) ||
             hasOuterParameter(dyn_cast<DeclaratorDecl>(declaration)) ||
             hasOuterParameter(dyn_cast<TagDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<FunctionDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<CXXRecordDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<VarDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<EnumDecl>(declaration));
    }

    bool hasVisibleSourceTemplateParameter(TemplateDecl const& owner,
                                           DeclarationName name,
                                           SourceLocation useLocation,
                                           SourceManager const& sourceManager)
    {
      auto const* parameters = owner.getTemplateParameters();

      if (parameters == nullptr)
      {
        return true;
      }

      return std::ranges::any_of(*parameters,
                                 [name, useLocation, &sourceManager](auto const* parameter)
                                 {
                                   if (parameter->getDeclName() != name)
                                   {
                                     return false;
                                   }

                                   auto const location = parameter->getSourceRange().getEnd();

                                   // A parameter enters scope after the complete parameter,
                                   // including its default argument. Uncertain source ordering
                                   // is not proof that a same-named parameter can be ignored.
                                   return location.isInvalid() || location.isMacroID() ||
                                          !sourceManager.isWrittenInSameFile(location, useLocation) ||
                                          sourceManager.isBeforeInTranslationUnit(location, useLocation);
                                 });
    }

    bool hasShadowingSourceTemplateParameter(SourceScopes const& scopes,
                                             DeclarationName name,
                                             SourceLocation useLocation,
                                             SourceManager const& sourceManager)
    {
      return std::ranges::any_of(
        scopes.owners,
        [name, useLocation, &sourceManager](auto const* owner)
        {
          if (auto const* templateOwner = dyn_cast<TemplateDecl>(owner); templateOwner != nullptr)
          {
            return hasVisibleSourceTemplateParameter(*templateOwner, name, useLocation, sourceManager);
          }

          // Keep definition-pattern and outer-list handling for source
          // function, record, variable, and enumeration declarations.
          return hasShadowingTemplateParameter(owner, name);
        });
    }

    bool isShadowedByCloserDeclaration(DeclarationName name,
                                       NestedNameSpecifierLoc const& lastRedundant,
                                       Decl const* enclosingDeclaration,
                                       DeclContext const* currentContext)
    {
      auto const* redundantNamespace = namespaceDecl(lastRedundant);

      if (name.isEmpty() || redundantNamespace == nullptr)
      {
        return false;
      }

      // A static member definition's outer lists belong to its VarDecl, which
      // is not itself a DeclContext. Inspect it before walking context parents.
      if (hasShadowingTemplateParameter(enclosingDeclaration, name))
      {
        return true;
      }

      auto hasPotentialShadow = [name](DeclContext const* context)
      {
        return std::ranges::any_of(context->decls(),
                                   [name](auto const* declaration)
                                   {
                                     auto const* named = dyn_cast<NamedDecl>(declaration);
                                     auto const* ns = dyn_cast<NamespaceDecl>(declaration);
                                     return isa<UsingDirectiveDecl>(declaration) || (ns != nullptr && ns->isInline()) ||
                                            (named != nullptr && named->getDeclName() == name);
                                   });
      };

      for (auto const* context = currentContext; context != nullptr && context != redundantNamespace;
           context = context->getParent())
      {
        if (hasShadowingTemplateParameter(Decl::castFromDeclContext(context), name))
        {
          return true;
        }

        // AST DeclContext lookup does not model inherited or using-directive
        // lookup. Without Sema, absence of a direct declaration is not proof.
        if (auto const* record = dyn_cast<CXXRecordDecl>(context); record != nullptr && record->getNumBases() != 0)
        {
          return true;
        }

        if (hasPotentialShadow(context))
        {
          return true;
        }

        // Only namespaces need redeclaration lookup; functions are not lookup contexts.
        if (auto const* ns = dyn_cast<NamespaceDecl>(context);
            ns != nullptr && (!ns->lookup(name).empty() || std::ranges::any_of(ns->redecls(), hasPotentialShadow)))
        {
          return true;
        }
      }

      return false;
    }

    bool hasInstantiatedSource(Decl const* declaration)
    {
      auto hasPattern = [](auto const* owner)
      { return owner != nullptr && owner->getTemplateInstantiationPattern() != nullptr; };
      return hasPattern(dyn_cast<FunctionDecl>(declaration)) || hasPattern(dyn_cast<CXXRecordDecl>(declaration)) ||
             hasPattern(dyn_cast<VarDecl>(declaration)) || hasPattern(dyn_cast<EnumDecl>(declaration));
    }

    bool hasAssociationFreeArguments(CallExpr const* call,
                                     DeclRefExpr const* reference,
                                     Decl const* enclosingDeclaration,
                                     DeclContext const* currentContext)
    {
      // Explicit template arguments are outside this source-type proof.
      // Substitution must not turn a dependent source argument into a seemingly
      // association-free type at the same location.
      if (reference->hasExplicitTemplateArgs() || call->isInstantiationDependent() ||
          hasInstantiatedSource(enclosingDeclaration))
      {
        return false;
      }

      for (auto const* context = currentContext; context != nullptr; context = context->getParent())
      {
        if (hasInstantiatedSource(Decl::castFromDeclContext(context)))
        {
          return false;
        }
      }

      return std::ranges::all_of(call->arguments(),
                                 [](auto const* argument)
                                 {
                                   // Default arguments do not participate in ADL. Inspect the source
                                   // expression, not the converted formal type: even IgnoreImpCasts
                                   // leaves an implicit conversion-function call returning a builtin.
                                   if (argument->isDefaultArgument())
                                   {
                                     return true;
                                   }

                                   auto const* source = argument->IgnoreImplicitAsWritten();
                                   auto const type = source->getType();
                                   return !source->isInstantiationDependent() && !type.isNull() &&
                                          type->isFundamentalType();
                                 });
    }

    bool hasSafeUnqualifiedFunctionUse(ASTContext& context,
                                       DeclRefExpr const* reference,
                                       Decl const* enclosingDeclaration,
                                       DeclContext const* currentContext)
    {
      Expr const* expression = reference;

      while (true)
      {
        auto const parents = context.getParents(*expression);

        if (parents.size() != 1)
        {
          return false;
        }

        auto const* parent = parents[0].get<Expr>();

        if (auto const* cast = dyn_cast_or_null<ImplicitCastExpr>(parent); cast != nullptr)
        {
          expression = cast;
          continue;
        }

        // Parenthesized callees and address-taking cannot enable ADL.
        if (isa_and_nonnull<ParenExpr>(parent))
        {
          return true;
        }

        if (auto const* unary = dyn_cast_or_null<UnaryOperator>(parent); unary != nullptr)
        {
          return unary->getOpcode() == UO_AddrOf;
        }

        if (auto const* call = dyn_cast_or_null<CallExpr>(parent); call != nullptr && call->getCallee() == expression)
        {
          return hasAssociationFreeArguments(call, reference, enclosingDeclaration, currentContext);
        }

        // Do not infer a callee's source syntax from an unfamiliar parent.
        return false;
      }
    }

    bool hasNamespaceDeclarationNamed(DeclContext const* context, DeclarationName name)
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

          if (hasNamespaceDeclarationNamed(dc, targetName))
          {
            return true;
          }
        }

        break;
      }

      return false;
    }

    bool tryExtractSpecLocAndNode(MatchFinder::MatchResult const& result,
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

    if (!tryExtractSpecLocAndNode(result, specLoc, node))
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

    auto const optSourceScopes = getSourceScopes(*result.Context, node);

    if (!optSourceScopes)
    {
      return;
    }

    struct EnclosingScope
    {
      Decl const* declaration;
      DeclContext const* context;
    };

    auto enclosingScopes = SmallVector<EnclosingScope, 4>{};

    for (auto const* declaration : optSourceScopes->enclosingDeclarations)
    {
      auto const* context = dyn_cast<DeclContext>(declaration);

      if (context == nullptr)
      {
        context = declaration->getDeclContext();
      }

      if (context == nullptr)
      {
        return;
      }

      enclosingScopes.push_back({.declaration = declaration, .context = context});
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
      if (!std::ranges::all_of(enclosingScopes,
                               [&specLocItem](auto const& scope)
                               { return isNamespaceRedundant(specLocItem, scope.context); }))
      {
        break;
      }

      lastRedundant = specLocItem;
    }

    if (!lastRedundant)
    {
      return;
    }

    // Removing q:: from q::Outer::Inner changes lookup of Outer, not Inner.
    // Require the source declaration in q and no uncertain or closer lookup.
    auto const* firstDeclaration = firstSurvivingDeclaration(result, lastRedundant, chain);

    if (!hasSameNamespaceLookup(firstDeclaration, lastRedundant) ||
        std::ranges::any_of(enclosingScopes,
                            [firstDeclaration, &lastRedundant](auto const& scope)
                            {
                              return isShadowedByCloserDeclaration(
                                firstDeclaration->getDeclName(), lastRedundant, scope.declaration, scope.context);
                            }))
    {
      return;
    }

    // Reject unknown source lookup and unconditional closer shadowing before
    // scanning intermediate namespace members. Every diagnostic still passes
    // the same guards; no lookup result is cached across AST queries.
    if (std::ranges::any_of(enclosingScopes,
                            [&lastRedundant, &chain](auto const& scope)
                            { return isShadowedByIntermediateNamespace(lastRedundant, chain, scope.context); }))
    {
      return;
    }

    // Requires-local declarations and variable/alias template parameters can
    // have namespace semantic contexts. Check every source-parent path too.
    if (hasShadowingSourceTemplateParameter(*optSourceScopes, firstDeclaration->getDeclName(), loc, sm))
    {
      return;
    }

    // Ordinary lookup alone is insufficient when complete removal turns a
    // namespace-qualified call into an unqualified call and enables ADL.
    // A surviving qualifier (including a record) still suppresses ADL.
    if (auto const* reference = result.Nodes.getNodeAs<DeclRefExpr>("declRef");
        reference != nullptr && isa<FunctionDecl>(reference->getDecl()) &&
        lastRedundant.getNestedNameSpecifier() == specLoc.getNestedNameSpecifier() &&
        std::ranges::any_of(
          enclosingScopes,
          [&result, reference](auto const& scope)
          { return !hasSafeUnqualifiedFunctionUse(*result.Context, reference, scope.declaration, scope.context); }))
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

    diag(loc, "redundant namespace qualification '%0'") << redundantText;
  }
} // namespace clang::tidy::readability
