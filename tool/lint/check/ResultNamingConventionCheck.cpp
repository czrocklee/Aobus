// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ResultNamingConventionCheck.h"

#include "check/FunctionNamingHelpers.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>

#include <algorithm>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isResultTemplate(TemplateDecl const* declaration)
    {
      return declaration != nullptr && declaration->getQualifiedNameAsString() == "ao::Result";
    }

    bool isResultType(QualType candidateType)
    {
      if (candidateType.isNull())
      {
        return false;
      }

      QualType const canonicalType = candidateType.getNonReferenceType().getCanonicalType().getUnqualifiedType();

      if (auto const* candidateRecordType = canonicalType->getAs<RecordType>(); candidateRecordType != nullptr)
      {
        auto const* specialization = dyn_cast<ClassTemplateSpecializationDecl>(candidateRecordType->getDecl());
        return specialization != nullptr && isResultTemplate(specialization->getSpecializedTemplate());
      }

      if (auto const* specializationType = canonicalType->getAs<TemplateSpecializationType>();
          specializationType != nullptr)
      {
        return isResultTemplate(specializationType->getTemplateName().getAsTemplateDecl());
      }

      if (auto const* deducedType = canonicalType->getAs<DeducedType>();
          deducedType != nullptr && !deducedType->getDeducedType().isNull())
      {
        return isResultType(deducedType->getDeducedType());
      }

      return false;
    }

    bool hasResultInitializerContract(Expr const& source, Expr const& instantiated)
    {
      if (isResultType(source.getType()))
      {
        return true;
      }

      auto const* sourceCall = dyn_cast<CallExpr>(source.IgnoreImplicitAsWritten());
      auto const* instantiatedCall = dyn_cast<CallExpr>(instantiated.IgnoreImplicitAsWritten());

      if (sourceCall == nullptr || instantiatedCall == nullptr || instantiatedCall->getDirectCallee() == nullptr)
      {
        return false;
      }

      FunctionDecl const& callee = function_naming::sourceFunctionDeclaration(*instantiatedCall->getDirectCallee());

      if (!isResultType(callee.getReturnType()))
      {
        return false;
      }

      // Resolve only a function named by the source lookup. An arbitrary callable
      // parameter gaining a Result-returning operator() at instantiation is not proof.
      auto const* lookup = dyn_cast<UnresolvedLookupExpr>(sourceCall->getCallee()->IgnoreParenImpCasts());
      return lookup != nullptr &&
             std::ranges::any_of(
               lookup->decls(),
               [&callee](NamedDecl const* declaration)
               {
                 declaration = declaration->getUnderlyingDecl();
                 auto const* functionTemplate = dyn_cast<FunctionTemplateDecl>(declaration);
                 auto const* function = functionTemplate == nullptr ? dyn_cast<FunctionDecl>(declaration)
                                                                    : functionTemplate->getTemplatedDecl();
                 return function != nullptr && &function_naming::sourceFunctionDeclaration(*function) == &callee;
               });
    }

    bool isResultName(StringRef name)
    {
      if (name == "res" || name == "_res")
      {
        return true;
      }

      return name.ends_with("Res") && name != "Res";
    }
  } // namespace

  void ResultNamingConventionCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(
      declaratorDecl(
        anyOf(declaratorDecl(unless(anyOf(hasAncestor(functionDecl(clang::ast_matchers::isTemplateInstantiation())),
                                          hasAncestor(cxxRecordDecl(clang::ast_matchers::isTemplateInstantiation())))))
                .bind("source_decl"),
              varDecl(unless(parmVarDecl()))))
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

    auto const* variable = dyn_cast<VarDecl>(resultDecl);
    bool const isAutoVariable = variable != nullptr && !isa<ParmVarDecl>(variable) &&
                                variable->getTypeSourceInfo() != nullptr &&
                                variable->getTypeSourceInfo()->getType()->getContainedAutoType() != nullptr;

    // Keep source evidence separate from resolved types; generic copies and
    // arbitrary callable results do not acquire a Result contract from callers.
    if (result.Nodes.getNodeAs<DeclaratorDecl>("source_decl") == nullptr)
    {
      if (isAutoVariable && variable->hasInit() && isResultType(variable->getType()))
      {
        _instantiatedAutoVariables.push_back(variable);
      }

      return;
    }

    if (isAutoVariable && variable->hasInit())
    {
      _sourceAutoVariables.try_emplace(variable->getLocation().getRawEncoding(), variable);
    }

    if (!isResultType(resultDecl->getType()) &&
        !(isAutoVariable && variable->hasInit() && isResultType(variable->getInit()->getType())))
    {
      return;
    }

    diagnoseDeclaration(*resultDecl);
  }

  void ResultNamingConventionCheck::onEndOfTranslationUnit()
  {
    for (VarDecl const* variable : _instantiatedAutoVariables)
    {
      auto const* source = _sourceAutoVariables.lookup(variable->getLocation().getRawEncoding());

      if (source != nullptr && hasResultInitializerContract(*source->getInit(), *variable->getInit()))
      {
        diagnoseDeclaration(*source);
      }
    }
  }

  void ResultNamingConventionCheck::diagnoseDeclaration(DeclaratorDecl const& declaration)
  {
    StringRef const name = declaration.getName();

    if (name.empty() || name == "_")
    {
      return;
    }

    // Instantiated locals retain their source location, including generic lambda
    // bodies instantiated through more than one enclosing function specialization.
    if (isResultName(name) || !_diagnosedLocations.insert(declaration.getLocation().getRawEncoding()).second)
    {
      return;
    }

    if (llvm::isa<FieldDecl>(declaration))
    {
      diag(declaration.getLocation(),
           "ao::Result field '%0' must be named 'res'/'_res' or end with 'Res' (for example, 'openRes' or "
           "'_openRes')")
        << name;
    }
    else if (llvm::isa<ParmVarDecl>(declaration))
    {
      diag(declaration.getLocation(), "ao::Result parameter '%0' must be named 'res' or end with 'Res'") << name;
    }
    else
    {
      diag(declaration.getLocation(), "ao::Result variable '%0' must be named 'res' or end with 'Res'") << name;
    }
  }
} // namespace clang::tidy::readability
