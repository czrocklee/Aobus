// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/AsyncFunctionNamingCheck.h"

#include "check/FunctionNamingHelpers.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isBoostAsioAwaitableTemplate(TemplateDecl const* declaration)
    {
      return declaration != nullptr && declaration->getQualifiedNameAsString() == "boost::asio::awaitable";
    }

    bool isTaskType(QualType candidateType)
    {
      if (candidateType.isNull() || candidateType->isReferenceType())
      {
        return false;
      }

      QualType const canonicalType = candidateType.getCanonicalType().getUnqualifiedType();

      if (auto const* candidateRecordType = canonicalType->getAs<RecordType>(); candidateRecordType != nullptr)
      {
        auto const* specialization = dyn_cast<ClassTemplateSpecializationDecl>(candidateRecordType->getDecl());
        return specialization != nullptr && isBoostAsioAwaitableTemplate(specialization->getSpecializedTemplate());
      }

      if (auto const* specializationType = canonicalType->getAs<TemplateSpecializationType>();
          specializationType != nullptr)
      {
        return isBoostAsioAwaitableTemplate(specializationType->getTemplateName().getAsTemplateDecl());
      }

      if (auto const* deducedType = canonicalType->getAs<DeducedType>();
          deducedType != nullptr && !deducedType->getDeducedType().isNull())
      {
        return isTaskType(deducedType->getDeducedType());
      }

      return false;
    }
  } // namespace

  void AsyncFunctionNamingCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(functionDecl().bind("function"), this);
  }

  void AsyncFunctionNamingCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* function = result.Nodes.getNodeAs<FunctionDecl>("function");

    if (function == nullptr || result.SourceManager == nullptr || !isTaskType(function->getReturnType()))
    {
      return;
    }

    FunctionDecl const& sourceFunction = function_naming::sourceFunctionDeclaration(*function);

    if (!function_naming::isProjectOwnedNamedFunction(sourceFunction, *result.SourceManager) ||
        function_naming::isFrameworkRequiredFunction(sourceFunction, *result.SourceManager) ||
        sourceFunction.getName().ends_with("Async") || !_diagnosedFunctions.insert(&sourceFunction).second)
    {
      return;
    }

    diag(sourceFunction.getLocation(), "Task-returning function '%0' must end with 'Async'")
      << sourceFunction.getName();
  }
} // namespace clang::tidy::readability
