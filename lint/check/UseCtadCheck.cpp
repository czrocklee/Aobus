// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseCtadCheck.h"

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
#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <cstdint>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    std::string getTemplateName(TemplateSpecializationTypeLoc const& tsLoc)
    {
      if (auto const* tst = tsLoc.getTypePtr())
      {
        if (auto const* tmpl = tst->getTemplateName().getAsTemplateDecl())
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
             name == "std::unordered_map" || name == "std::unordered_multimap" ||
             (ref.starts_with("std::") && ref.ends_with("_distribution"));
    }

    std::uint32_t countSourceArgs(CXXConstructExpr const* construct)
    {
      if (construct == nullptr)
      {
        return 0;
      }

      std::uint32_t count = 0;

      for (auto const* arg : construct->arguments())
      {
        if (arg != nullptr && !isa<CXXDefaultArgExpr>(arg))
        {
          ++count;
        }
      }

      return count;
    }

    Expr const* getSourceArg(CXXConstructExpr const* construct, std::uint32_t index)
    {
      if (construct == nullptr)
      {
        return nullptr;
      }

      std::uint32_t sourceIndex = 0;

      for (auto const* arg : construct->arguments())
      {
        if (arg == nullptr || isa<CXXDefaultArgExpr>(arg))
        {
          continue;
        }

        if (sourceIndex == index)
        {
          return arg;
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

      if (auto const* initList = dyn_cast<InitListExpr>(expr))
      {
        return initList;
      }

      for (auto const* child : expr->children())
      {
        if (auto const* childExpr = dyn_cast_or_null<Expr>(child))
        {
          if (auto const* initList = findInitListExpr(childExpr))
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

      auto const* sourceArg = getSourceArg(construct, 0);

      if (sourceArg == nullptr)
      {
        return true;
      }

      auto const* sourceExpr = sourceArg->IgnoreImplicit();

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
      auto const* sourceArg = getSourceArg(construct, 0);
      auto const* initList = findInitListExpr(sourceArg);

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

      auto const* sourceArg0 = getSourceArg(construct, 0);
      auto const* sourceArg1 = getSourceArg(construct, 1);

      if (sourceArg0 == nullptr || sourceArg1 == nullptr)
      {
        return false;
      }

      auto const* arg0 = sourceArg0->IgnoreImplicit();
      auto const* arg1 = sourceArg1->IgnoreImplicit();

      if (arg0 == nullptr || arg1 == nullptr)
      {
        return false;
      }

      QualType const t0 = arg0->getType().getCanonicalType();
      QualType const t1 = arg1->getType().getCanonicalType();

      return (t0->isPointerType() || t0->isNullPtrType()) && t1->isIntegerType();
    }

    bool isParenSizeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || construct->isListInitialization() || countSourceArgs(construct) != 2 ||
          !isSequenceTemplateName(getConstructedTemplateName(construct)))
      {
        return false;
      }

      auto const* sourceArg0 = getSourceArg(construct, 0);

      if (sourceArg0 == nullptr)
      {
        return false;
      }

      auto const* arg0 = sourceArg0->IgnoreImplicit();

      if (arg0 == nullptr)
      {
        return false;
      }

      return arg0->getType().getCanonicalType()->isIntegerType();
    }

    bool isSingleSizeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || construct->isStdInitListInitialization() || countSourceArgs(construct) != 1 ||
          !isSequenceTemplateName(getConstructedTemplateName(construct)))
      {
        return false;
      }

      auto const* sourceArg0 = getSourceArg(construct, 0);

      if (sourceArg0 == nullptr)
      {
        return false;
      }

      auto const* arg0 = sourceArg0->IgnoreImplicit();

      if (arg0 == nullptr)
      {
        return false;
      }

      return arg0->getType().getCanonicalType()->isIntegerType();
    }

    bool isSingleSameTypeConstructor(CXXConstructExpr const* construct)
    {
      if (construct == nullptr || countSourceArgs(construct) != 1)
      {
        return false;
      }

      auto const* sourceArg0 = getSourceArg(construct, 0);

      if (sourceArg0 == nullptr)
      {
        return false;
      }

      auto const* arg0 = sourceArg0->IgnoreImplicit();

      if (arg0 == nullptr)
      {
        return false;
      }

      return hasSameUnqualifiedCanonicalType(arg0->getType(), construct->getType());
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
        auto const* sourceArg = getSourceArg(construct, i);

        if (targetType.isNull() || sourceArg == nullptr)
        {
          return true;
        }

        if (auto const* sourceExpr = sourceArg->IgnoreImplicit();
            sourceExpr == nullptr || !hasSameUnqualifiedCanonicalType(sourceExpr->getType(), targetType))
        {
          return true;
        }
      }

      return false;
    }

    bool isUnsafeForCtad(CXXConstructExpr const* construct)
    {
      auto const templateName = getConstructedTemplateName(construct);

      return isAlwaysUnsafeTemplateName(templateName) || isPointerSizeConstructor(construct) ||
             isParenSizeConstructor(construct) || isSingleSizeConstructor(construct) ||
             isSingleSameTypeConstructor(construct) || isPairWithTypeChangingArgs(construct) ||
             isInitializerListWithTypeChangingElements(construct);
    }
  } // namespace

  void UseCtadCheck::registerMatchers(MatchFinder* finder)
  {
    auto explicitTemplateTypeLoc = elaboratedTypeLoc(hasNamedTypeLoc(templateSpecializationTypeLoc()));

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

    if (auto const* tempObj = result.Nodes.getNodeAs<CXXTemporaryObjectExpr>("temp_obj"))
    {
      if (isUnsafeForCtad(tempObj))
      {
        return;
      }

      auto const loc = tempObj->getBeginLoc();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      auto const typeLoc = tempObj->getTypeSourceInfo()->getTypeLoc();
      auto const elabLoc = typeLoc.getAs<ElaboratedTypeLoc>();

      if (elabLoc.isNull())
      {
        return;
      }

      auto const tsLoc = elabLoc.getNamedTypeLoc().getAs<TemplateSpecializationTypeLoc>();

      if (tsLoc.isNull())
      {
        return;
      }

      auto const templateName = getTemplateName(tsLoc);

      diag(
        loc, "consider using CTAD (Class Template Argument Deduction) instead of explicit template arguments '%0<...>'")
        << templateName;
    }
    else if (auto const* varDecl = result.Nodes.getNodeAs<VarDecl>("var_decl"))
    {
      auto const* init = result.Nodes.getNodeAs<CXXConstructExpr>("var_init");

      if (init == nullptr || isUnsafeForCtad(init))
      {
        return;
      }

      auto const loc = varDecl->getBeginLoc();

      if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return;
      }

      auto const typeLoc = varDecl->getTypeSourceInfo()->getTypeLoc();
      auto const elabLoc = typeLoc.getAs<ElaboratedTypeLoc>();

      if (elabLoc.isNull())
      {
        return;
      }

      auto const tsLoc = elabLoc.getNamedTypeLoc().getAs<TemplateSpecializationTypeLoc>();

      if (tsLoc.isNull())
      {
        return;
      }

      auto const templateName = getTemplateName(tsLoc);

      diag(
        loc, "consider using CTAD (Class Template Argument Deduction) instead of explicit template arguments '%0<...>'")
        << templateName;
    }
  }
} // namespace clang::tidy::readability
