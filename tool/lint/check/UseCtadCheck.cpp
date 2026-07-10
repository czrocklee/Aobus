// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseCtadCheck.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Type.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <cstdint>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    TemplateSpecializationTypeLoc getExplicitTemplateTypeLoc(TypeLoc typeLoc)
    {
      return typeLoc.getUnqualifiedLoc().getAs<TemplateSpecializationTypeLoc>();
    }

    std::string getTemplateName(TemplateSpecializationTypeLoc const& tsLoc)
    {
      if (auto const* tst = tsLoc.getTypePtr(); tst != nullptr)
      {
        if (auto const* tmpl = tst->getTemplateName().getAsTemplateDecl(); tmpl != nullptr)
        {
          return tmpl->getQualifiedNameAsString();
        }
      }

      return "";
    }

    ClassTemplateSpecializationDecl const* getTemplateSpecialization(QualType type)
    {
      if (type.isNull())
      {
        return nullptr;
      }

      auto const* record = type.getCanonicalType()->getAsCXXRecordDecl();

      if (record == nullptr)
      {
        return nullptr;
      }

      return dyn_cast<ClassTemplateSpecializationDecl>(record);
    }

    std::string getConstructedTemplateName(CXXConstructExpr const* construct)
    {
      if (construct == nullptr)
      {
        return "";
      }

      auto const* spec = getTemplateSpecialization(construct->getType());

      if (spec == nullptr)
      {
        return "";
      }

      return spec->getSpecializedTemplate()->getQualifiedNameAsString();
    }

    QualType getTemplateArgumentType(CXXConstructExpr const* construct, std::uint32_t index)
    {
      auto const* spec = getTemplateSpecialization(construct->getType());

      if (spec == nullptr)
      {
        return {};
      }

      auto const& args = spec->getTemplateArgs();

      if (args.size() <= index || args[index].getKind() != TemplateArgument::Type)
      {
        return {};
      }

      return args[index].getAsType();
    }

    QualType getFirstTemplateArgumentType(CXXConstructExpr const* construct)
    {
      return getTemplateArgumentType(construct, 0);
    }

    bool isSequenceTemplateName(std::string const& name)
    {
      return name == "std::vector" || name == "std::deque" || name == "std::list" || name == "std::forward_list" ||
             name == "std::basic_string";
    }

    bool isAlwaysUnsafeTemplateName(std::string const& name)
    {
      auto const ref = llvm::StringRef{name};

      return name == "std::optional" || name == "std::span" || name == "std::unique_ptr" ||
             name == "llvm::StringSwitch" || name == "std::map" || name == "std::multimap" ||
             name == "std::unordered_map" || name == "std::unordered_multimap" || name == "ao::Result" ||
             (ref.starts_with("std::") && ref.ends_with("_distribution"));
    }

    std::uint32_t countSourceArgs(CXXConstructExpr const* construct)
    {
      if (construct == nullptr)
      {
        return 0;
      }

      std::uint32_t count = 0;

      for (auto const* argument : construct->arguments())
      {
        if (argument != nullptr && !isa<CXXDefaultArgExpr>(argument))
        {
          ++count;
        }
      }

      return count;
    }

    Expr const* getSourceArgument(CXXConstructExpr const* construct, std::uint32_t index)
    {
      if (construct == nullptr)
      {
        return nullptr;
      }

      std::uint32_t sourceIndex = 0;

      for (auto const* argument : construct->arguments())
      {
        if (argument == nullptr || isa<CXXDefaultArgExpr>(argument))
        {
          continue;
        }

        if (sourceIndex == index)
        {
          return argument;
        }

        ++sourceIndex;
      }

      return nullptr;
    }

    InitListExpr const* findInitListExpr(Expr const* expr)
    {
      if (expr == nullptr)
      {
        return nullptr;
      }

      if (auto const* initList = dyn_cast<InitListExpr>(expr); initList != nullptr)
      {
        return initList;
      }

      for (auto const* child : expr->children())
      {
        if (auto const* childExpr = dyn_cast_or_null<Expr>(child); childExpr != nullptr)
        {
          if (auto const* initList = findInitListExpr(childExpr); initList != nullptr)
          {
            return initList;
          }
        }
      }

      return nullptr;
    }

    bool hasSameUnqualifiedCanonicalType(QualType lhs, QualType rhs)
    {
      if (lhs.isNull() || rhs.isNull())
      {
        return false;
      }

      return lhs.getCanonicalType().getUnqualifiedType() == rhs.getCanonicalType().getUnqualifiedType();
    }

    bool isFixedWidthIntegerTypeSpelling(llvm::StringRef spelling)
    {
      return spelling.contains("int8_t") || spelling.contains("int16_t") || spelling.contains("int32_t") ||
             spelling.contains("int64_t") || spelling.contains("uint8_t") || spelling.contains("uint16_t") ||
             spelling.contains("uint32_t") || spelling.contains("uint64_t");
    }

    bool hasFixedWidthIntegerTemplateArgument(TemplateSpecializationTypeLoc tsLoc,
                                              SourceManager const& sm,
                                              LangOptions const& langOpts)
    {
      for (std::uint32_t i = 0; i < tsLoc.getNumArgs(); ++i)
      {
        auto const argumentLoc = tsLoc.getArgLoc(i);

        if (argumentLoc.getArgument().getKind() != TemplateArgument::Type)
        {
          continue;
        }

        auto const range = argumentLoc.getSourceRange();

        if (range.isInvalid())
        {
          continue;
        }

        auto const spelling = Lexer::getSourceText(CharSourceRange::getTokenRange(range), sm, langOpts);

        if (isFixedWidthIntegerTypeSpelling(spelling))
        {
          return true;
        }
      }

      return false;
    }

    bool isTypeChangingInitializerElement(Expr const* init, QualType valueType)
    {
      if (init == nullptr)
      {
        return true;
      }

      auto const* expr = init->IgnoreImplicit();

      if (expr == nullptr || !hasSameUnqualifiedCanonicalType(expr->getType(), valueType))
      {
        return true;
      }

      if (isa<InitListExpr>(expr))
      {
        return true;
      }

      auto const* construct = dyn_cast<CXXConstructExpr>(expr);

      if (construct == nullptr || isa<CXXTemporaryObjectExpr>(construct))
      {
        return false;
      }

      if (countSourceArgs(construct) != 1)
      {
        return true;
      }

      auto const* sourceArgument = getSourceArgument(construct, 0);

      if (sourceArgument == nullptr)
      {
        return true;
      }

      auto const* sourceExpr = sourceArgument->IgnoreImplicit();

      return sourceExpr == nullptr || !hasSameUnqualifiedCanonicalType(sourceExpr->getType(), valueType);
    }

    bool isInitializerListWithTypeChangingElements(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || !construct->isStdInitListInitialization())
      {
        return false;
      }

      auto const templateName = getConstructedTemplateName(construct);

      if (templateName != "std::vector" && templateName != "std::set" && templateName != "std::multiset" &&
          templateName != "std::flat_set")
      {
        return false;
      }

      QualType const valueType = getFirstTemplateArgumentType(construct);
      auto const* sourceArgument = getSourceArgument(construct, 0);
      auto const* initList = findInitListExpr(sourceArgument);

      if (valueType.isNull() || initList == nullptr)
      {
        return true;
      }

      return std::ranges::any_of(initList->inits(),
                                 [valueType](auto const* init)
                                 { return init == nullptr || isTypeChangingInitializerElement(init, valueType); });
    }

    bool isPointerSizeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || countSourceArgs(construct) != 2)
      {
        return false;
      }

      auto const* firstSourceArgument = getSourceArgument(construct, 0);
      auto const* secondSourceArgument = getSourceArgument(construct, 1);

      if (firstSourceArgument == nullptr || secondSourceArgument == nullptr)
      {
        return false;
      }

      auto const* firstExpression = firstSourceArgument->IgnoreImplicit();
      auto const* secondExpression = secondSourceArgument->IgnoreImplicit();

      if (firstExpression == nullptr || secondExpression == nullptr)
      {
        return false;
      }

      QualType const firstType = firstExpression->getType().getCanonicalType();
      QualType const secondType = secondExpression->getType().getCanonicalType();

      return (firstType->isPointerType() || firstType->isNullPtrType()) && secondType->isIntegerType();
    }

    bool isParenSizeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || construct->isListInitialization() || countSourceArgs(construct) != 2 ||
          !isSequenceTemplateName(getConstructedTemplateName(construct)))
      {
        return false;
      }

      auto const* firstSourceArgument = getSourceArgument(construct, 0);

      if (firstSourceArgument == nullptr)
      {
        return false;
      }

      auto const* firstExpression = firstSourceArgument->IgnoreImplicit();

      if (firstExpression == nullptr)
      {
        return false;
      }

      return firstExpression->getType().getCanonicalType()->isIntegerType();
    }

    bool isSingleSizeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || construct->isStdInitListInitialization() || countSourceArgs(construct) != 1 ||
          !isSequenceTemplateName(getConstructedTemplateName(construct)))
      {
        return false;
      }

      auto const* firstSourceArgument = getSourceArgument(construct, 0);

      if (firstSourceArgument == nullptr)
      {
        return false;
      }

      auto const* firstExpression = firstSourceArgument->IgnoreImplicit();

      if (firstExpression == nullptr)
      {
        return false;
      }

      return firstExpression->getType().getCanonicalType()->isIntegerType();
    }

    bool isSingleSameTypeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || countSourceArgs(construct) != 1)
      {
        return false;
      }

      auto const* firstSourceArgument = getSourceArgument(construct, 0);

      if (firstSourceArgument == nullptr)
      {
        return false;
      }

      auto const* firstExpression = firstSourceArgument->IgnoreImplicit();

      if (firstExpression == nullptr)
      {
        return false;
      }

      return hasSameUnqualifiedCanonicalType(firstExpression->getType(), construct->getType());
    }

    bool isPairWithTypeChangingArgs(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || getConstructedTemplateName(construct) != "std::pair")
      {
        return false;
      }

      auto const sourceArgCount = countSourceArgs(construct);

      if (sourceArgCount != 2)
      {
        return false;
      }

      for (std::uint32_t i = 0; i < sourceArgCount; ++i)
      {
        QualType const targetType = getTemplateArgumentType(construct, i);
        auto const* sourceArgument = getSourceArgument(construct, i);

        if (targetType.isNull() || sourceArgument == nullptr)
        {
          return true;
        }

        if (auto const* sourceExpr = sourceArgument->IgnoreImplicit();
            sourceExpr == nullptr || !hasSameUnqualifiedCanonicalType(sourceExpr->getType(), targetType))
        {
          return true;
        }
      }

      return false;
    }

    bool isInitializerListConstructor(CXXConstructorDecl const* ctor)
    {
      if (ctor->getNumParams() == 0)
      {
        return false;
      }

      QualType parameterType = ctor->getParamDecl(0)->getType();

      if (parameterType->isReferenceType())
      {
        parameterType = parameterType->getPointeeType();
      }

      auto const* record = parameterType->getAsCXXRecordDecl();

      return record != nullptr && record->getQualifiedNameAsString() == "std::initializer_list";
    }

    bool hasInitializerListConstructor(CXXConstructExpr const* construct)
    {
      auto const* ctor = construct->getConstructor();

      if (ctor == nullptr)
      {
        return false;
      }

      auto const* record = ctor->getParent();

      if (record == nullptr)
      {
        return false;
      }

      return std::ranges::any_of(
        record->ctors(), [](CXXConstructorDecl const* ctor) { return isInitializerListConstructor(ctor); });
    }

    constexpr std::int32_t kMaxTypeReferencesDepth = 64;

    bool referencesTemplateParameter(QualType type, TemplateTypeParmDecl const* parameter, std::int32_t depth = 0);

    bool referencesTemplateParameter(TemplateSpecializationType const* tst,
                                     TemplateTypeParmDecl const* parameter,
                                     std::int32_t depth)
    {
      if (tst == nullptr)
      {
        return false;
      }

      return std::ranges::any_of(tst->template_arguments(),
                                 [parameter, depth](TemplateArgument const& argument)
                                 {
                                   return argument.getKind() == TemplateArgument::Type &&
                                          referencesTemplateParameter(argument.getAsType(), parameter, depth + 1);
                                 });
    }

    bool referencesTemplateParameter(ClassTemplateSpecializationDecl const* spec,
                                     TemplateTypeParmDecl const* parameter,
                                     std::int32_t depth)
    {
      if (spec == nullptr)
      {
        return false;
      }

      auto const& args = spec->getTemplateArgs();

      for (std::uint32_t i = 0; i < args.size(); ++i)
      {
        if (args[i].getKind() == TemplateArgument::Type &&
            referencesTemplateParameter(args[i].getAsType(), parameter, depth + 1))
        {
          return true;
        }
      }

      return false;
    }

    bool isNonAliasTemplateSpecialization(TemplateSpecializationType const* tst)
    {
      if (tst == nullptr)
      {
        return false;
      }

      auto const* tmpl = tst->getTemplateName().getAsTemplateDecl();
      return tmpl != nullptr && !isa<TypeAliasTemplateDecl>(tmpl);
    }

    // Recursively check whether a QualType references a given TemplateTypeParmDecl.
    bool referencesTemplateParameter(QualType type, TemplateTypeParmDecl const* parameter, std::int32_t depth)
    {
      if (type.isNull() || parameter == nullptr || depth > kMaxTypeReferencesDepth)
      {
        return false;
      }

      type = type.getNonReferenceType().getUnqualifiedType();

      // Direct match: the type *is* the template parameter.
      if (auto const* parmType = type->getAs<TemplateTypeParmType>(); parmType != nullptr)
      {
        return parmType->getDepth() == parameter->getDepth() && parmType->getIndex() == parameter->getIndex();
      }

      if (auto const* substParmType = type->getAs<SubstTemplateTypeParmType>(); substParmType != nullptr)
      {
        auto const* replaced = substParmType->getReplacedParameter();

        if (replaced == nullptr)
        {
          return false;
        }

        return (replaced->getDepth() == parameter->getDepth() && replaced->getIndex() == parameter->getIndex()) ||
               referencesTemplateParameter(substParmType->getReplacementType(), parameter, depth + 1);
      }

      // Pointer / array element type.
      if (type->isPointerType() || type->isArrayType())
      {
        return referencesTemplateParameter(
          type->getPointeeOrArrayElementType()->getCanonicalTypeInternal(), parameter, depth + 1);
      }

      // Template specialization: check each template argument recursively.
      // Alias templates are not generally deducible in CTAD, so be conservative and do not
      // consider a parameter deducible merely because it appears in an alias template argument list.
      if (auto const* tst = type->getAs<TemplateSpecializationType>();
          isNonAliasTemplateSpecialization(tst) && referencesTemplateParameter(tst, parameter, depth))
      {
        return true;
      }

      // Also try the desugared RecordType path (for type aliases / elaborated types).
      if (auto const* spec = getTemplateSpecialization(type); referencesTemplateParameter(spec, parameter, depth))
      {
        return true;
      }

      auto const desugared = type->getLocallyUnqualifiedSingleStepDesugaredType();

      return desugared != type && referencesTemplateParameter(desugared, parameter, depth + 1);
    }

    bool isSameTemplateArgument(TemplateArgument const& lhs, TemplateArgument const& rhs)
    {
      if (lhs.getKind() != rhs.getKind())
      {
        return false;
      }

      if (lhs.getKind() == TemplateArgument::Type)
      {
        // Compare canonical types exactly (preserving cv-qualifiers). Using the unqualified
        // comparison would treat e.g. int and int const as the same default argument, which
        // could lead to an unsafe CTAD suggestion.
        return lhs.getAsType().getCanonicalType() == rhs.getAsType().getCanonicalType();
      }

      // Conservative for non-type/template-template arguments: treat them as different from
      // the default so that explicit non-type/template-template arguments suppress the check.
      return false;
    }

    // Conservative check: if any explicitly-written template argument is a non-type argument,
    // suppress the CTAD suggestion. Determining whether non-type arguments are deducible from
    // constructor parameters is complex and error-prone, so we err on the side of avoiding
    // unsafe suggestions.
    bool hasExplicitNonTypeArgument(ClassTemplateSpecializationDecl const* spec, std::uint32_t explicitTemplateArgCount)
    {
      if (spec == nullptr || explicitTemplateArgCount == 0)
      {
        return false;
      }

      auto const& args = spec->getTemplateArgs();

      for (std::uint32_t i = 0; i < explicitTemplateArgCount && i < args.size(); ++i)
      {
        // Any argument that is not a type argument (and not a pack, which is handled separately)
        // is considered non-deducible: non-type and template-template arguments cannot reliably
        // be checked for deducibility from constructor parameters.
        if (auto const kind = args[i].getKind(); kind != TemplateArgument::Type && kind != TemplateArgument::Pack)
        {
          return true;
        }
      }

      return false;
    }

    bool isExplicitNonDefaultTemplateArgument(ClassTemplateSpecializationDecl const* spec,
                                              TemplateTypeParmDecl const* typeParm,
                                              std::uint32_t explicitTemplateArgCount)
    {
      auto const parameterIndex = typeParm->getIndex();

      if (parameterIndex >= explicitTemplateArgCount)
      {
        return false;
      }

      if (!typeParm->hasDefaultArgument())
      {
        return true;
      }

      auto const& args = spec->getTemplateArgs();

      if (args.size() <= parameterIndex)
      {
        return true;
      }

      return !isSameTemplateArgument(args[parameterIndex], typeParm->getDefaultArgument().getArgument());
    }

    CXXConstructorDecl const* getPrimaryConstructorPattern(CXXConstructorDecl const* ctor)
    {
      if (auto const* memberPattern = ctor->getInstantiatedFromMemberFunction(); memberPattern != nullptr)
      {
        if (auto const* tmplCtor = dyn_cast<CXXConstructorDecl>(memberPattern); tmplCtor != nullptr)
        {
          return tmplCtor;
        }
      }
      else if (auto const* pattern = ctor->getTemplateInstantiationPattern(); pattern != nullptr)
      {
        if (auto const* tmplCtor = dyn_cast<CXXConstructorDecl>(pattern); tmplCtor != nullptr)
        {
          return tmplCtor;
        }
      }

      return ctor;
    }

    bool isPackDeducibleViaConstructor(CXXConstructorDecl const* primaryCtor)
    {
      if (auto const* ctorTemplate = primaryCtor->getDescribedTemplate(); ctorTemplate != nullptr)
      {
        auto const* ctorParameters = ctorTemplate->getTemplateParameters();

        if (ctorParameters != nullptr)
        {
          return std::ranges::any_of(*ctorParameters,
                                     [](NamedDecl const* ctorParameter)
                                     {
                                       auto const* ctorTypeParm = dyn_cast<TemplateTypeParmDecl>(ctorParameter);
                                       return ctorTypeParm != nullptr && ctorTypeParm->isParameterPack();
                                     });
        }
      }

      return false;
    }

    bool isTypeParameterDeducibleFromConstructor(CXXConstructorDecl const* primaryCtor,
                                                 TemplateTypeParmDecl const* typeParm)
    {
      for (std::uint32_t i = 0; i < primaryCtor->getNumParams(); ++i)
      {
        if (referencesTemplateParameter(primaryCtor->getParamDecl(i)->getType(), typeParm))
        {
          return true;
        }
      }

      return false;
    }

    // Check whether an explicitly-written class template argument is not deducible from the
    // constructor's parameter types. When such an argument is unreachable, CTAD may select a
    // default or a deduction guide that differs from the explicit specialization.
    bool hasExplicitNonDeducibleTemplateParameter(CXXConstructExpr const* construct,
                                                  std::uint32_t explicitTemplateArgCount)
    {
      if (construct == nullptr)
      {
        return false;
      }

      auto const* ctor = construct->getConstructor();

      if (ctor == nullptr)
      {
        return false;
      }

      auto const* spec = getTemplateSpecialization(construct->getType());

      if (spec == nullptr)
      {
        return false;
      }

      auto const* tmpl = spec->getSpecializedTemplate();

      if (tmpl == nullptr)
      {
        return false;
      }

      auto const* parameterList = tmpl->getTemplateParameters();

      if (parameterList == nullptr)
      {
        return false;
      }

      // Conservative: non-type arguments are difficult to verify for deducibility, so
      // suppress the warning whenever they are explicitly provided.
      if (hasExplicitNonTypeArgument(spec, explicitTemplateArgCount))
      {
        return true;
      }

      // Walk up to the primary (un-instantiated) constructor declaration.
      // The instantiated constructor's parameter types are already substituted
      // (e.g. `int` instead of `T`), so we need the templated pattern.
      auto const* primaryCtor = getPrimaryConstructorPattern(ctor);

      return std::ranges::any_of(
        *parameterList,
        [&](NamedDecl const* namedDecl)
        {
          auto const* typeParm = dyn_cast<TemplateTypeParmDecl>(namedDecl);

          if (typeParm == nullptr || !isExplicitNonDefaultTemplateArgument(spec, typeParm, explicitTemplateArgCount))
          {
            return false;
          }

          // For parameter packs, assume they are deducible when the constructor is itself a
          // member template with a parameter pack (e.g. std::tuple's converting constructor).
          // Otherwise, check whether the class template pack is referenced in the constructor
          // parameter types.
          if (typeParm->isParameterPack() && isPackDeducibleViaConstructor(primaryCtor))
          {
            return false;
          }

          return !isTypeParameterDeducibleFromConstructor(primaryCtor, typeParm);
        });
    }

    bool isUnsafeForCtad(CXXConstructExpr const* construct, std::uint32_t explicitTemplateArgCount)
    {
      if (construct == nullptr)
      {
        return true;
      }

      auto const templateName = getConstructedTemplateName(construct);

      // Braced initialization that resolved to a non-initializer-list constructor
      // on a type that also has an initializer_list constructor: after removing
      // the template arguments, CTAD would re-resolve the braced list to the
      // initializer_list constructor and silently change the meaning
      // (e.g. std::vector<int>{it1, it2} to std::vector{it1, it2}).
      if (construct->isListInitialization() && !construct->isStdInitListInitialization() &&
          hasInitializerListConstructor(construct))
      {
        return true;
      }

      // std::pair is excluded from the generic non-deducible-parameter check because it is
      // already covered by isPairWithTypeChangingArgs, which understands pair-specific CTAD.
      return isAlwaysUnsafeTemplateName(templateName) ||
             (templateName != "std::pair" &&
              hasExplicitNonDeducibleTemplateParameter(construct, explicitTemplateArgCount)) ||
             isPointerSizeConstructor(construct) || isParenSizeConstructor(construct) ||
             isSingleSizeConstructor(construct) || isSingleSameTypeConstructor(construct) ||
             isPairWithTypeChangingArgs(construct) || isInitializerListWithTypeChangingElements(construct);
    }

    void reportCtadWarning(ClangTidyCheck& check, SourceLocation loc, TemplateSpecializationTypeLoc tsLoc)
    {
      auto const templateName = getTemplateName(tsLoc);
      auto diagBuilder =
        check.diag(
          loc,
          "consider using CTAD (Class Template Argument Deduction) instead of explicit template arguments '%0<...>'")
        << templateName;

      if (tsLoc.getLAngleLoc().isValid() && tsLoc.getRAngleLoc().isValid())
      {
        diagBuilder << FixItHint::CreateRemoval(SourceRange{tsLoc.getLAngleLoc(), tsLoc.getRAngleLoc()});
      }
    }
  } // namespace

  void UseCtadCheck::registerMatchers(MatchFinder* finder)
  {
    auto explicitTemplateTypeLoc = templateSpecializationTypeLoc();

    finder->addMatcher(cxxTemporaryObjectExpr(unless(isExpansionInSystemHeader()),
                                              hasTypeLoc(explicitTemplateTypeLoc),
                                              hasAnyArgument(unless(cxxDefaultArgExpr())))
                         .bind("temp_obj"),
                       this);

    finder->addMatcher(
      varDecl(unless(isExpansionInSystemHeader()),
              unless(isImplicit()),
              unless(parmVarDecl()),
              hasTypeLoc(explicitTemplateTypeLoc),
              hasInitializer(cxxConstructExpr(hasAnyArgument(unless(cxxDefaultArgExpr()))).bind("var_init")))
        .bind("var_decl"),
      this);
  }

  void UseCtadCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    if (auto const* tempObj = result.Nodes.getNodeAs<CXXTemporaryObjectExpr>("temp_obj"); tempObj != nullptr)
    {
      auto const loc = tempObj->getBeginLoc();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      auto const tsLoc = getExplicitTemplateTypeLoc(tempObj->getTypeSourceInfo()->getTypeLoc());

      if (tsLoc.isNull())
      {
        return;
      }

      if (isUnsafeForCtad(tempObj, tsLoc.getNumArgs()))
      {
        return;
      }

      if (hasFixedWidthIntegerTemplateArgument(tsLoc, sm, result.Context->getLangOpts()))
      {
        return;
      }

      reportCtadWarning(*this, loc, tsLoc);
    }
    else if (auto const* varDecl = result.Nodes.getNodeAs<VarDecl>("var_decl"); varDecl != nullptr)
    {
      auto const* init = result.Nodes.getNodeAs<CXXConstructExpr>("var_init");

      if (init == nullptr)
      {
        return;
      }

      auto const loc = varDecl->getBeginLoc();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      auto const tsLoc = getExplicitTemplateTypeLoc(varDecl->getTypeSourceInfo()->getTypeLoc());

      if (tsLoc.isNull())
      {
        return;
      }

      if (isUnsafeForCtad(init, tsLoc.getNumArgs()))
      {
        return;
      }

      if (hasFixedWidthIntegerTemplateArgument(tsLoc, sm, result.Context->getLangOpts()))
      {
        return;
      }

      reportCtadWarning(*this, loc, tsLoc);
    }
  }
} // namespace clang::tidy::readability
