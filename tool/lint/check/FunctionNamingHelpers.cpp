// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/FunctionNamingHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringRef.h>

#include <algorithm>

namespace clang::tidy::readability::function_naming
{
  namespace
  {
    bool hasForeignOverride(CXXMethodDecl const& method, SourceManager const& sourceManager)
    {
      return std::ranges::any_of(
        method.overridden_methods(),
        [&sourceManager](CXXMethodDecl const* overridden)
        {
          SourceLocation const location = sourceManager.getExpansionLoc(overridden->getLocation());
          return sourceManager.isInSystemHeader(location) || sourceManager.isInExternCSystemHeader(location) ||
                 hasForeignOverride(*overridden, sourceManager);
        });
    }

    ClassTemplateSpecializationDecl const* asClassTemplateSpecialization(CXXRecordDecl const& record)
    {
      return dyn_cast<ClassTemplateSpecializationDecl>(record.getDefinition() == nullptr ? &record
                                                                                         : record.getDefinition());
    }

    bool isSpecializationOf(CXXRecordDecl const& record, StringRef qualifiedTemplateName)
    {
      auto const* specialization = asClassTemplateSpecialization(record);
      auto const* classTemplate = specialization == nullptr ? nullptr : specialization->getSpecializedTemplate();
      return classTemplate != nullptr && classTemplate->getQualifiedNameAsString() == qualifiedTemplateName;
    }

    CXXRecordDecl const* findRecursiveAstVisitorBase(CXXRecordDecl const& record)
    {
      for (CXXBaseSpecifier const& base : record.bases())
      {
        // A dependent CRTP base has template identity but no record declaration yet.
        if (auto const* specialization = base.getType()->getAs<TemplateSpecializationType>(); specialization != nullptr)
        {
          auto const* classTemplate =
            dyn_cast_or_null<ClassTemplateDecl>(specialization->getTemplateName().getAsTemplateDecl());

          if (classTemplate != nullptr && classTemplate->getQualifiedNameAsString() == "clang::RecursiveASTVisitor")
          {
            return classTemplate->getTemplatedDecl()->getDefinition();
          }
        }

        CXXRecordDecl const* baseRecord = base.getType()->getAsCXXRecordDecl();

        if (baseRecord == nullptr)
        {
          continue;
        }

        if (isSpecializationOf(*baseRecord, "clang::RecursiveASTVisitor"))
        {
          auto const* specialization = asClassTemplateSpecialization(*baseRecord);
          return specialization->getSpecializedTemplate()->getTemplatedDecl()->getDefinition();
        }

        if (CXXRecordDecl const* nestedBase = findRecursiveAstVisitorBase(*baseRecord); nestedBase != nullptr)
        {
          return nestedBase;
        }
      }

      return nullptr;
    }

    bool hasRavCustomizationName(StringRef name)
    {
      return (name.starts_with("Visit") && name.size() > StringRef{"Visit"}.size()) ||
             (name.starts_with("Traverse") && name.size() > StringRef{"Traverse"}.size()) ||
             (name.starts_with("WalkUpFrom") && name.size() > StringRef{"WalkUpFrom"}.size()) ||
             name == "dataTraverseStmtPre" || name == "dataTraverseStmtPost";
    }

    bool hasCompatibleParameters(CXXMethodDecl const& method, CXXMethodDecl const& frameworkMethod)
    {
      if (method.param_size() > frameworkMethod.param_size() || method.isConst() != frameworkMethod.isConst() ||
          method.getRefQualifier() != frameworkMethod.getRefQualifier())
      {
        return false;
      }

      auto const* methodParameter = method.param_begin();
      auto const* frameworkParameter = frameworkMethod.param_begin();

      for (; methodParameter != method.param_end(); ++methodParameter, ++frameworkParameter)
      {
        if ((*methodParameter)->getType().getCanonicalType() != (*frameworkParameter)->getType().getCanonicalType())
        {
          return false;
        }
      }

      for (; frameworkParameter != frameworkMethod.param_end(); ++frameworkParameter)
      {
        if (!(*frameworkParameter)->hasDefaultArg())
        {
          return false;
        }
      }

      return true;
    }

    bool isRecursiveAstVisitorCustomization(CXXMethodDecl const& method)
    {
      if (!hasRavCustomizationName(method.getName()) || !method.getReturnType()->isBooleanType())
      {
        return false;
      }

      auto const* parent = method.getParent();
      CXXRecordDecl const* ravBase = parent == nullptr ? nullptr : findRecursiveAstVisitorBase(*parent);

      if (ravBase == nullptr)
      {
        return false;
      }

      DeclContext::lookup_result const candidates = ravBase->lookup(method.getDeclName());
      return std::ranges::any_of(candidates,
                                 [&method](NamedDecl const* declaration)
                                 {
                                   auto const* frameworkMethod = dyn_cast<CXXMethodDecl>(declaration);
                                   return frameworkMethod != nullptr &&
                                          frameworkMethod->getReturnType()->isBooleanType() &&
                                          hasCompatibleParameters(method, *frameworkMethod);
                                 });
    }

    bool isWinrtImplementsSpecialization(CXXRecordDecl const& record)
    {
      return isSpecializationOf(record, "winrt::implements");
    }

    bool isWinrtInterfaceType(QualType type, StringRef qualifiedTemplateName, QualType elementType)
    {
      QualType const canonicalType = type.getCanonicalType().getUnqualifiedType();
      TemplateDecl const* declaration = nullptr;
      auto arguments = ArrayRef<TemplateArgument>{};

      if (auto const* specializationType = canonicalType->getAs<TemplateSpecializationType>();
          specializationType != nullptr)
      {
        declaration = specializationType->getTemplateName().getAsTemplateDecl();
        arguments = specializationType->template_arguments();
      }
      else if (auto const* recordType = canonicalType->getAs<RecordType>(); recordType != nullptr)
      {
        auto const* specialization = dyn_cast<ClassTemplateSpecializationDecl>(recordType->getDecl());

        if (specialization != nullptr)
        {
          declaration = specialization->getSpecializedTemplate();
          arguments = specialization->getTemplateArgs().asArray();
        }
      }

      return declaration != nullptr && declaration->getQualifiedNameAsString() == qualifiedTemplateName &&
             (elementType.isNull() ||
              (!arguments.empty() && arguments.front().getKind() == TemplateArgument::Type &&
               arguments.front().getAsType().getCanonicalType().getUnqualifiedType() == elementType));
    }

    bool hasWinrtInterfaceInTemplateArgument(TemplateArgument const& argument,
                                             StringRef qualifiedTemplateName,
                                             QualType elementType)
    {
      if (argument.getKind() == TemplateArgument::Type)
      {
        return isWinrtInterfaceType(argument.getAsType(), qualifiedTemplateName, elementType);
      }

      if (argument.getKind() == TemplateArgument::Pack)
      {
        return std::ranges::any_of(
          argument.pack_elements(),
          [&qualifiedTemplateName, elementType](TemplateArgument const& element)
          { return hasWinrtInterfaceInTemplateArgument(element, qualifiedTemplateName, elementType); });
      }

      return false;
    }

    bool hasWinrtInterface(CXXRecordDecl const& record, StringRef qualifiedTemplateName, QualType elementType = {})
    {
      for (CXXBaseSpecifier const& base : record.bases())
      {
        CXXRecordDecl const* baseRecord = base.getType()->getAsCXXRecordDecl();

        if (baseRecord == nullptr)
        {
          continue;
        }

        if (isWinrtImplementsSpecialization(*baseRecord))
        {
          auto const* specialization = asClassTemplateSpecialization(*baseRecord);

          if (specialization != nullptr)
          {
            ArrayRef<TemplateArgument> const arguments = specialization->getTemplateArgs().asArray();

            if (std::ranges::any_of(
                  arguments.drop_front(),
                  [&qualifiedTemplateName, elementType](TemplateArgument const& argument)
                  { return hasWinrtInterfaceInTemplateArgument(argument, qualifiedTemplateName, elementType); }))
            {
              return true;
            }
          }
        }

        if (hasWinrtInterface(*baseRecord, qualifiedTemplateName, elementType))
        {
          return true;
        }
      }

      return false;
    }

    bool isUint32Reference(QualType type, ASTContext const& context)
    {
      auto const* reference = type->getAs<LValueReferenceType>();

      if (reference == nullptr)
      {
        return false;
      }

      QualType const valueType = reference->getPointeeType();
      return !valueType.isConstQualified() && valueType->isUnsignedIntegerType() &&
             context.getTypeSize(valueType) == 32;
    }

    bool isWinrtStandardInterfaceMethod(CXXMethodDecl const& method)
    {
      if (!method.getReturnType()->isBooleanType())
      {
        return false;
      }

      auto const* parent = method.getParent();

      if (parent == nullptr)
      {
        return false;
      }

      StringRef const name = method.getName();

      if ((name == "HasCurrent" || name == "MoveNext") && method.param_empty())
      {
        return hasWinrtInterface(*parent, "winrt::Windows::Foundation::Collections::IIterator");
      }

      if (name != "IndexOf" || method.param_size() != 2 || method.isStatic() || method.isVolatile() ||
          method.getRefQualifier() != RQ_None ||
          !isUint32Reference(method.getParamDecl(1)->getType(), method.getASTContext()))
      {
        return false;
      }

      QualType const parameterType = method.getParamDecl(0)->getType();

      // Projection inputs accept the interface element by value or const lvalue reference.
      if (parameterType->isRValueReferenceType() || parameterType.getNonReferenceType().isVolatileQualified() ||
          (parameterType->isLValueReferenceType() && !parameterType.getNonReferenceType().isConstQualified()))
      {
        return false;
      }

      QualType const elementType = parameterType.getNonReferenceType().getCanonicalType().getUnqualifiedType();
      return hasWinrtInterface(*parent, "winrt::Windows::Foundation::Collections::IVectorView", elementType);
    }
  } // namespace

  bool isProjectOwnedNamedFunction(FunctionDecl const& function, SourceManager const& sourceManager)
  {
    SourceLocation const location = sourceManager.getExpansionLoc(function.getLocation());
    return !function.isImplicit() && function.getIdentifier() != nullptr && !function.getName().empty() &&
           !location.isInvalid() && !sourceManager.isInSystemHeader(location) &&
           !sourceManager.isInExternCSystemHeader(location);
  }

  FunctionDecl const& sourceFunctionDeclaration(FunctionDecl const& function)
  {
    FunctionDecl const* sourceFunction = &function;

    while (true)
    {
      if (FunctionDecl const* pattern = sourceFunction->getTemplateInstantiationPattern();
          pattern != nullptr && pattern != sourceFunction)
      {
        sourceFunction = pattern;
        continue;
      }

      if (FunctionDecl const* memberPattern = sourceFunction->getInstantiatedFromMemberFunction();
          memberPattern != nullptr && memberPattern != sourceFunction)
      {
        sourceFunction = memberPattern;
        continue;
      }

      return *sourceFunction->getCanonicalDecl();
    }
  }

  bool isFrameworkRequiredFunction(FunctionDecl const& function, SourceManager const& sourceManager)
  {
    auto const* method = dyn_cast<CXXMethodDecl>(&function);
    return method != nullptr &&
           (hasForeignOverride(*method, sourceManager) || isRecursiveAstVisitorCustomization(*method) ||
            isWinrtStandardInterfaceMethod(*method));
  }
} // namespace clang::tidy::readability::function_naming
