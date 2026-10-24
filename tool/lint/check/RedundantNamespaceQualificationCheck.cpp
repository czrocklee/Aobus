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
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ExprConcepts.h>
#include <clang/AST/NestedNameSpecifier.h> // NOLINT(misc-include-cleaner) -- inline location accessors
#include <clang/AST/NestedNameSpecifierBase.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
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
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>

#include <algorithm>
#include <cstdint>
#include <optional>

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::readability
{
  namespace
  {
    bool hasInstantiatedSource(Decl const* declaration);

    bool isSourceLocusBeforeUse(SourceLocation locus, SourceLocation useLocation, SourceManager const& sourceManager)
    {
      return locus.isInvalid() || locus.isMacroID() || !sourceManager.isWrittenInSameFile(locus, useLocation) ||
             sourceManager.isBeforeInTranslationUnit(locus, useLocation);
    }

    bool isUseWithinFunctionBody(Decl const* declaration,
                                 SourceLocation useLocation,
                                 SourceManager const& sourceManager)
    {
      auto const* function = dyn_cast_or_null<FunctionDecl>(declaration);

      if (function == nullptr || !function->doesThisDeclarationHaveABody() || function->getBody() == nullptr)
      {
        return false;
      }

      auto const range = function->getBody()->getSourceRange();
      return range.isValid() && !range.getBegin().isMacroID() && !range.getEnd().isMacroID() &&
             sourceManager.isWrittenInSameFile(range.getBegin(), useLocation) &&
             sourceManager.isWrittenInSameFile(range.getEnd(), useLocation) &&
             sourceManager.isPointWithin(useLocation, range.getBegin(), range.getEnd());
    }

    bool hasFunctionBodyLookupOwners(DeclContext const* context,
                                     SourceLocation useLocation,
                                     SourceManager const& sourceManager)
    {
      if (!isa_and_nonnull<FunctionDecl>(context))
      {
        return false;
      }

      for (auto const* current = context; current != nullptr; current = current->getParent())
      {
        auto const* function = dyn_cast<FunctionDecl>(current);

        if (function == nullptr)
        {
          continue;
        }

        // Lambda semantic parents can skip signature and template owners,
        // even inside an ordinary function. Recover their written ancestry.
        if (auto const* method = dyn_cast<CXXMethodDecl>(function);
            (method != nullptr && method->getParent()->isLambda()) ||
            !isUseWithinFunctionBody(function, useLocation, sourceManager))
        {
          return false;
        }
      }

      return true;
    }

    bool canAffectLookup(NamedDecl const& declaration, bool hasSurvivingQualifier)
    {
      if (!hasSurvivingQualifier)
      {
        return true;
      }

      if (auto const* shadow = dyn_cast<UsingShadowDecl>(&declaration); shadow != nullptr)
      {
        return canAffectLookup(*shadow->getTargetDecl(), true);
      }

      if (isa<NamespaceDecl, NamespaceAliasDecl, TypeDecl, UnresolvedUsingTypenameDecl>(declaration))
      {
        return true;
      }

      if (auto const* templateDeclaration = dyn_cast<TemplateDecl>(&declaration);
          templateDeclaration != nullptr && templateDeclaration->getTemplatedDecl() != nullptr)
      {
        return isa<TypeDecl>(templateDeclaration->getTemplatedDecl());
      }

      return isa<TemplateTemplateParmDecl, UsingDecl>(declaration);
    }

    bool canHideTemplateParameterAt(NamedDecl const& declaration,
                                    DeclarationName name,
                                    SourceLocation useLocation,
                                    SourceManager const& sourceManager,
                                    bool hasSurvivingQualifier)
    {
      return declaration.getDeclName() == name && canAffectLookup(declaration, hasSurvivingQualifier) &&
             isSourceLocusBeforeUse(declaration.getSourceRange().getEnd(), useLocation, sourceManager);
    }

    bool canHideFunctionParameterAt(ParmVarDecl const& declaration,
                                    DeclarationName name,
                                    SourceLocation useLocation,
                                    SourceManager const& sourceManager,
                                    bool hasSurvivingQualifier)
    {
      // A normal parameter becomes visible after its complete declarator, before
      // its default argument. ParmVarDecl::getSourceRange includes that default.
      return declaration.getDeclName() == name && !hasSurvivingQualifier &&
             isSourceLocusBeforeUse(declaration.DeclaratorDecl::getSourceRange().getEnd(), useLocation, sourceManager);
    }

    bool hasShadowingFunctionParameter(Decl const* declaration,
                                       DeclarationName name,
                                       SourceLocation useLocation,
                                       SourceManager const& sourceManager,
                                       bool hasSurvivingQualifier)
    {
      auto const* function = dyn_cast_or_null<FunctionDecl>(declaration);
      return function != nullptr &&
             std::ranges::any_of(function->parameters(),
                                 [name, useLocation, &sourceManager, hasSurvivingQualifier](auto const* parameter)
                                 {
                                   return canHideFunctionParameterAt(
                                     *parameter, name, useLocation, sourceManager, hasSurvivingQualifier);
                                 });
    }

    bool canHideEnumeratorAt(EnumConstantDecl const& declaration,
                             DeclarationName name,
                             SourceLocation useLocation,
                             SourceManager const& sourceManager,
                             bool hasSurvivingQualifier)
    {
      return declaration.getDeclName() == name && !hasSurvivingQualifier &&
             isSourceLocusBeforeUse(declaration.getSourceRange().getEnd(), useLocation, sourceManager);
    }

    class SourceQuery final
    {
    public:
      using VisitStates = llvm::DenseMap<DynTypedNode, bool>;

      SourceQuery(ASTContext& context, DynTypedNode const& rootNode)
        : _context{context}, _sourceManager{context.getSourceManager()}, _rootNode{rootNode}
      {
      }

      bool hasNearestRequiresExpression() const { return _nearestHasRequiresExpression; }

      std::optional<SmallVector<Decl const*, 8>> sourceDeclarations(DynTypedNode const& node,
                                                                    bool nearestOnly,
                                                                    DeclarationName unqualifiedName = {},
                                                                    SourceLocation useLocation = {})
      {
        // Spelled-only matching avoids repeated callbacks, but source-owner
        // recovery needs wrappers such as a generic lambda's outer initializer.
        auto const traversal = TraversalKindScope{_context, TK_AsIs};
        auto declarations = SmallVector<Decl const*, 8>{};
        auto visited = VisitStates{};

        if (nearestOnly)
        {
          _fullSourceReady = false;
          _nearestHasRequiresExpression = false;
        }

        _sourceScopes.clear();

        bool const foundSource =
          tryVisitSourceNode(node, nearestOnly, unqualifiedName, useLocation, declarations, visited) &&
          !declarations.empty();

        if (!nearestOnly)
        {
          _fullSourceReady = true;
          _fullSourceValid = foundSource;
        }

        if (!foundSource)
        {
          return std::nullopt;
        }

        return declarations;
      }

      bool hasActiveLocalEnumerator(EnumDecl const& enumeration,
                                    DeclarationName name,
                                    SourceLocation useLocation,
                                    bool hasSurvivingQualifier)
      {
        if (hasSurvivingQualifier || enumeration.isScoped() ||
            !std::ranges::any_of(
              enumeration.enumerators(),
              [name, useLocation, this](auto const* enumerator)
              { return canHideEnumeratorAt(*enumerator, name, useLocation, _sourceManager, false); }))
        {
          return false;
        }

        return !hasCompleteSourceAncestry(name, useLocation) || isLocalEnumScopeActive(enumeration);
      }

    private:
      bool isSupportedSourceNode(DynTypedNode const& node) const
      {
        return node.get<Decl>() != nullptr || node.get<Stmt>() != nullptr || node.get<TypeLoc>() != nullptr ||
               node.get<NestedNameSpecifierLoc>() != nullptr || node.get<CXXCtorInitializer>() != nullptr ||
               node.get<Attr>() != nullptr;
      }

      bool isReusedDefaultEdge(DynTypedNode const& node) const
      {
        // These edges reuse declaration-site expressions at callers. They do
        // not introduce another lexical lookup scope for the written name.
        return node.get<CXXDefaultArgExpr>() != nullptr || node.get<CXXDefaultInitExpr>() != nullptr;
      }

      void recordSourceNode(DynTypedNode const& node, SourceLocation useLocation)
      {
        if (auto const* statement = node.get<Stmt>();
            statement != nullptr && isa<CompoundStmt, IfStmt, SwitchStmt, ForStmt, CXXForRangeStmt>(statement) &&
            isUseWithinSourceRange(statement->getSourceRange(), useLocation))
        {
          _sourceScopes.insert(statement);
        }
      }

      bool isUseWithinSourceRange(SourceRange range, SourceLocation useLocation) const
      {
        auto const begin = range.getBegin();
        auto const end = range.getEnd();
        return begin.isInvalid() || end.isInvalid() || begin.isMacroID() || end.isMacroID() ||
               !_sourceManager.isWrittenInSameFile(begin, useLocation) ||
               !_sourceManager.isWrittenInSameFile(end, useLocation) ||
               _sourceManager.isPointWithin(useLocation, begin, end);
      }

      bool hasShadowingRequiresParameter(DynTypedNode const& node,
                                         DeclarationName name,
                                         SourceLocation useLocation) const
      {
        auto const* requirement = node.get<RequiresExpr>();
        return requirement != nullptr && !name.isEmpty() &&
               std::ranges::any_of(
                 requirement->getLocalParameters(),
                 [name, useLocation, this](auto const* parameter)
                 { return canHideFunctionParameterAt(*parameter, name, useLocation, _sourceManager, false); });
      }

      bool tryVisitSourceNode(DynTypedNode const& node,
                              bool nearestOnly,
                              DeclarationName unqualifiedName,
                              SourceLocation useLocation,
                              SmallVector<Decl const*, 8>& declarations,
                              VisitStates& visited)
      {
        if (isReusedDefaultEdge(node))
        {
          return true;
        }

        if (!isSupportedSourceNode(node))
        {
          return false;
        }

        if (auto const found = visited.find(node); found != visited.end())
        {
          return found->second;
        }

        // false denotes an active path; true denotes a completed node. Do not
        // retain a DenseMap iterator while recursive insertion can rehash it.
        visited.try_emplace(node, false);
        auto const* declaration = node.get<Decl>();

        if (declaration != nullptr && isInstantiationCopy(declaration))
        {
          visited[node] = true;
          return true;
        }

        recordSourceNode(node, useLocation);

        if (nearestOnly && node.get<RequiresExpr>() != nullptr)
        {
          _nearestHasRequiresExpression = true;
        }

        if (hasShadowingRequiresParameter(node, unqualifiedName, useLocation))
        {
          return false;
        }

        if (declaration != nullptr)
        {
          declarations.push_back(declaration);

          if (nearestOnly)
          {
            visited[node] = true;
            return true;
          }

          if (isa<NamespaceDecl, TranslationUnitDecl>(declaration))
          {
            visited[node] = true;
            return true;
          }
        }

        auto const parents = _context.getParents(node);

        if (parents.empty())
        {
          return false;
        }

        for (auto const& parent : parents)
        {
          if (!tryVisitSourceNode(parent, nearestOnly, unqualifiedName, useLocation, declarations, visited))
          {
            return false;
          }
        }

        visited[node] = true;
        return true;
      }

      bool hasCompleteSourceAncestry(DeclarationName name, SourceLocation useLocation)
      {
        if (!_fullSourceReady && !sourceDeclarations(_rootNode, false, name, useLocation))
        {
          return false;
        }

        return _fullSourceValid;
      }

      bool hasInstantiatedOwner(DeclContext const* context)
      {
        if (context == nullptr)
        {
          return false;
        }

        if (auto const found = _instantiatedOwners.find(context); found != _instantiatedOwners.end())
        {
          return found->second;
        }

        bool const instantiated =
          hasInstantiatedSource(Decl::castFromDeclContext(context)) || hasInstantiatedOwner(context->getParent());
        _instantiatedOwners.try_emplace(context, instantiated);
        return instantiated;
      }

      bool isInstantiationCopy(Decl const* declaration)
      {
        return hasInstantiatedSource(declaration) || hasInstantiatedOwner(declaration->getDeclContext());
      }

      bool isLocalEnumScopeActive(EnumDecl const& declaration)
      {
        auto const traversal = TraversalKindScope{_context, TK_AsIs};
        auto const parents = _context.getParents(declaration);

        if (parents.size() != 1)
        {
          return true;
        }

        auto const* declarationStatement = parents[0].get<DeclStmt>();

        if (declarationStatement == nullptr)
        {
          return true;
        }

        auto const scopes = _context.getParents(*declarationStatement);

        if (scopes.size() != 1)
        {
          return true;
        }

        auto const* scope = scopes[0].get<Stmt>();

        if (scope == nullptr || !isa<CompoundStmt, IfStmt, SwitchStmt, ForStmt, CXXForRangeStmt>(scope))
        {
          return true;
        }

        return _sourceScopes.contains(scope);
      }

      ASTContext& _context;
      SourceManager const& _sourceManager;
      llvm::DenseMap<DeclContext const*, bool> _instantiatedOwners;
      llvm::SmallPtrSet<Stmt const*, 16> _sourceScopes;
      DynTypedNode _rootNode;
      bool _fullSourceReady{false};
      bool _fullSourceValid{false};
      bool _nearestHasRequiresExpression{false};
    };

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

    bool hasShadowingTemplateParameter(Decl const* declaration,
                                       DeclarationName name,
                                       SourceLocation useLocation,
                                       SourceManager const& sourceManager,
                                       bool hasSurvivingQualifier)
    {
      if (declaration == nullptr)
      {
        return false;
      }

      auto hasNamedParameter =
        [name, useLocation, &sourceManager, hasSurvivingQualifier](TemplateParameterList const* parameters)
      {
        return parameters != nullptr &&
               std::ranges::any_of(*parameters,
                                   [name, useLocation, &sourceManager, hasSurvivingQualifier](auto const* parameter)
                                   {
                                     return canHideTemplateParameterAt(
                                       *parameter, name, useLocation, sourceManager, hasSurvivingQualifier);
                                   });
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

      auto hasPatternParameter =
        [declaration, name, useLocation, &sourceManager, hasSurvivingQualifier](auto const* owner)
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

        return pattern != nullptr && pattern != declaration &&
               hasShadowingTemplateParameter(pattern, name, useLocation, sourceManager, hasSurvivingQualifier);
      };

      auto const* templateDeclaration = dyn_cast<TemplateDecl>(declaration);

      // Concepts and leading requires-clauses are owned by TemplateDecl,
      // rather than the templated declaration or another DeclContext.
      // Out-of-line definitions can rename an enclosing class template's
      // parameters. Instantiated nodes share their source locations but may
      // omit those outer lists, so also inspect the source definition pattern.
      return (templateDeclaration != nullptr &&
              (hasNamedParameter(templateDeclaration->getTemplateParameters()) ||
               hasShadowingTemplateParameter(
                 templateDeclaration->getTemplatedDecl(), name, useLocation, sourceManager, hasSurvivingQualifier))) ||
             hasNamedParameter(declaration->getDescribedTemplateParams()) ||
             hasOuterParameter(dyn_cast<DeclaratorDecl>(declaration)) ||
             hasOuterParameter(dyn_cast<TagDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<FunctionDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<CXXRecordDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<VarDecl>(declaration)) ||
             hasPatternParameter(dyn_cast<EnumDecl>(declaration));
    }

    class LookupShadowQuery final
    {
    public:
      LookupShadowQuery(DeclarationName name,
                        NestedNameSpecifierLoc const& lastRedundant,
                        Decl const* enclosingDeclaration,
                        SourceLocation useLocation,
                        SourceManager const& sourceManager,
                        bool hasSurvivingQualifier,
                        SourceQuery& sourceQuery)
        : _name{name}
        , _redundantNamespace{namespaceDecl(lastRedundant)}
        , _enclosingDeclaration{enclosingDeclaration}
        , _useLocation{useLocation}
        , _sourceManager{sourceManager}
        , _hasSurvivingQualifier{hasSurvivingQualifier}
        , _sourceQuery{sourceQuery}
      {
      }

      bool hasShadowingDeclaration(DeclContext const* currentContext)
      {
        if (_name.isEmpty() || _redundantNamespace == nullptr)
        {
          return false;
        }

        // A static member definition's outer lists belong to its VarDecl,
        // which is not itself a DeclContext.
        if (hasShadowingTemplateParameter(
              _enclosingDeclaration, _name, _useLocation, _sourceManager, _hasSurvivingQualifier))
        {
          return true;
        }

        for (auto const* context = currentContext;
             context != nullptr && context->getRedeclContext() != _redundantNamespace->getRedeclContext();
             context = context->getLookupParent())
        {
          if (hasShadowingContextDeclaration(context))
          {
            return true;
          }
        }

        return false;
      }

    private:
      bool hasShadowingContextDeclaration(DeclContext const* context)
      {
        return hasShadowingTemplateParameter(
                 Decl::castFromDeclContext(context), _name, _useLocation, _sourceManager, _hasSurvivingQualifier) ||
               hasFunctionScopeShadow(dyn_cast<FunctionDecl>(context)) ||
               hasRequiresScopeShadow(dyn_cast<RequiresExprBodyDecl>(context)) ||
               hasRecordScopeUncertainty(dyn_cast<CXXRecordDecl>(context)) || hasPotentialShadow(context) ||
               hasNamespaceShadow(dyn_cast<NamespaceDecl>(context));
      }

      bool hasShadowingCapture(CXXRecordDecl const* record) const
      {
        return !_hasSurvivingQualifier && record != nullptr && record->isLambda() &&
               std::ranges::any_of(
                 record->captures(),
                 [this](auto const& capture)
                 { return capture.capturesVariable() && capture.getCapturedVar()->getDeclName() == _name; });
      }

      bool canCandidateHide(NamedDecl const* declaration, DeclContext const* context)
      {
        if (declaration == nullptr || declaration->getDeclName() != _name ||
            !canAffectLookup(*declaration, _hasSurvivingQualifier) || isa<ParmVarDecl>(declaration))
        {
          return false;
        }

        if (auto const* enumerator = dyn_cast<EnumConstantDecl>(declaration);
            enumerator != nullptr && isa<EnumDecl>(context))
        {
          return canHideEnumeratorAt(*enumerator, _name, _useLocation, _sourceManager, _hasSurvivingQualifier);
        }

        // Class lookup must also agree with lookup in the completed class,
        // even outside a complete-class context ([class.member.lookup]). A
        // later member therefore forbids shortening, not just an earlier one.
        return true;
      }

      bool hasPotentialShadow(DeclContext const* context)
      {
        if (auto const* record = dyn_cast<CXXRecordDecl>(context); record != nullptr)
        {
          return std::ranges::any_of(record->lookup(_name),
                                     [this, context](auto const* declaration)
                                     { return canCandidateHide(declaration, context); });
        }

        return std::ranges::any_of(context->decls(),
                                   [this, context](auto const* declaration)
                                   { return canContextDeclarationHide(declaration, context); });
      }

      bool canContextDeclarationHide(Decl const* declaration, DeclContext const* context)
      {
        // Inspect local enumerators during the existing declaration scan,
        // rather than rescanning every local for every qualified reference.
        if (auto const* enumeration = dyn_cast<EnumDecl>(declaration);
            enumeration != nullptr && isa<FunctionDecl>(context) &&
            _sourceQuery.hasActiveLocalEnumerator(*enumeration, _name, _useLocation, _hasSurvivingQualifier))
        {
          return true;
        }

        auto const* ns = dyn_cast<NamespaceDecl>(declaration);
        return isa<UsingDirectiveDecl>(declaration) || (ns != nullptr && ns->isInline()) ||
               canCandidateHide(dyn_cast<NamedDecl>(declaration), context);
      }

      bool hasFunctionScopeShadow(FunctionDecl const* function)
      {
        if (function == nullptr)
        {
          return false;
        }

        bool const hasParameter =
          hasShadowingFunctionParameter(function, _name, _useLocation, _sourceManager, _hasSurvivingQualifier);
        auto const* method = dyn_cast<CXXMethodDecl>(function);
        bool const hasCapture =
          method != nullptr && method->getParent()->isLambda() && hasShadowingCapture(method->getParent());
        return hasParameter || hasCapture;
      }

      bool hasRequiresScopeShadow(RequiresExprBodyDecl const* requirement) const
      {
        return requirement != nullptr &&
               std::ranges::any_of(requirement->decls(),
                                   [this](auto const* declaration)
                                   {
                                     auto const* parameter = dyn_cast<ParmVarDecl>(declaration);
                                     return parameter != nullptr &&
                                            canHideFunctionParameterAt(
                                              *parameter, _name, _useLocation, _sourceManager, _hasSurvivingQualifier);
                                   });
      }

      bool hasRecordScopeUncertainty(CXXRecordDecl const* record) const
      {
        return record != nullptr && (record->getNumBases() != 0 || hasShadowingCapture(record));
      }

      bool hasNamespaceShadow(NamespaceDecl const* ns)
      {
        if (ns == nullptr)
        {
          return false;
        }

        // Namespace lookup includes transparent unscoped enumerations and
        // declarations from reopened namespace blocks.
        bool const lookupHides = std::ranges::any_of(ns->lookup(_name),
                                                     [this](auto const* declaration)
                                                     { return canAffectLookup(*declaration, _hasSurvivingQualifier); });
        return lookupHides ||
               std::ranges::any_of(
                 ns->redecls(), [this](auto const* redeclaration) { return hasPotentialShadow(redeclaration); });
      }

      DeclarationName _name;
      NamespaceDecl const* _redundantNamespace;
      Decl const* _enclosingDeclaration;
      SourceLocation _useLocation;
      SourceManager const& _sourceManager;
      bool _hasSurvivingQualifier;
      SourceQuery& _sourceQuery;
    };

    bool hasCloserShadowingDeclaration(DeclarationName name,
                                       NestedNameSpecifierLoc const& lastRedundant,
                                       Decl const* enclosingDeclaration,
                                       DeclContext const* currentContext,
                                       SourceLocation useLocation,
                                       SourceManager const& sourceManager,
                                       bool hasSurvivingQualifier,
                                       SourceQuery& sourceQuery)
    {
      return LookupShadowQuery{
        name, lastRedundant, enclosingDeclaration, useLocation, sourceManager, hasSurvivingQualifier, sourceQuery}
        .hasShadowingDeclaration(currentContext);
    }

    bool hasInstantiatedSource(Decl const* declaration)
    {
      auto hasPattern = [](auto const* owner)
      {
        // A primary variable template can report itself as its pattern.
        // That is written source, not an instantiation copy.
        return owner != nullptr && owner->getTemplateInstantiationPattern() != nullptr &&
               owner->getTemplateInstantiationPattern() != owner;
      };
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

    // Diagnose written expressions once, not their default-use or implicit
    // instantiation copies. Lookup proofs below still reject dependent calls.
    finder->addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, declRefExpr().bind("declRef")), this);
    finder->addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, typeLoc().bind("typeLoc")), this);
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

    auto sourceQuery = SourceQuery{*result.Context, node};
    auto const optEnclosingDeclarations = sourceQuery.sourceDeclarations(node, true, {}, loc);

    if (!optEnclosingDeclarations)
    {
      return;
    }

    struct EnclosingScope
    {
      Decl const* declaration;
      DeclContext const* context;
    };

    auto enclosingScopes = SmallVector<EnclosingScope, 4>{};

    for (auto const* declaration : *optEnclosingDeclarations)
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

    if (!hasSameNamespaceLookup(firstDeclaration, lastRedundant))
    {
      return;
    }

    // Only a potentially redundant qualifier needs the outer source-owner
    // proof. Template parameter defaults and requires bodies can have semantic
    // namespace contexts, so nearest DeclContext alone is insufficient.
    bool const hasSurvivingQualifier = lastRedundant.getNestedNameSpecifier() != specLoc.getNestedNameSpecifier();
    auto const unqualifiedName = hasSurvivingQualifier ? DeclarationName{} : firstDeclaration->getDeclName();
    bool const useHasFunctionBodyOwners =
      !sourceQuery.hasNearestRequiresExpression() &&
      std::ranges::all_of(
        enclosingScopes, [loc, &sm](auto const& scope) { return hasFunctionBodyLookupOwners(scope.context, loc, sm); });
    auto const optOwners = useHasFunctionBodyOwners ? optEnclosingDeclarations
                                                    : sourceQuery.sourceDeclarations(node, false, unqualifiedName, loc);

    if (!optOwners ||
        std::ranges::any_of(*optOwners,
                            [firstDeclaration, loc, &sm, hasSurvivingQualifier](auto const* owner)
                            {
                              return hasShadowingTemplateParameter(
                                       owner, firstDeclaration->getDeclName(), loc, sm, hasSurvivingQualifier) ||
                                     hasShadowingFunctionParameter(
                                       owner, firstDeclaration->getDeclName(), loc, sm, hasSurvivingQualifier);
                            }) ||
        std::ranges::any_of(
          enclosingScopes,
          [firstDeclaration, &lastRedundant, loc, &sm, hasSurvivingQualifier, &sourceQuery](auto const& scope)
          {
            return hasCloserShadowingDeclaration(firstDeclaration->getDeclName(),
                                                 lastRedundant,
                                                 scope.declaration,
                                                 scope.context,
                                                 loc,
                                                 sm,
                                                 hasSurvivingQualifier,
                                                 sourceQuery);
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
