// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/BoolFunctionNamingCheck.h"

#include "check/FunctionNamingHelpers.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/CharInfo.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/Specifiers.h>

#include <algorithm>
#include <array>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    FunctionDecl const& boolSourceDeclaration(FunctionDecl const& function)
    {
      // An explicitly written specialization owns its contract independently.
      if (function.getTemplateSpecializationKind() == TSK_ExplicitSpecialization)
      {
        return *function.getCanonicalDecl();
      }

      return function_naming::sourceFunctionDeclaration(function);
    }

    bool hasSourceBoolExpression(Expr const* expression)
    {
      if (expression == nullptr)
      {
        return false;
      }

      if (expression->getType()->isBooleanType())
      {
        return true;
      }

      auto const* call = dyn_cast<CallExpr>(expression->IgnoreParenImpCasts());
      auto const* lookup =
        call == nullptr ? nullptr : dyn_cast<UnresolvedLookupExpr>(call->getCallee()->IgnoreParenImpCasts());

      // Only source lookup with no ADL can fix the complete candidate set.
      // Dependent operators and arbitrary callable parameters are not proof.
      return lookup != nullptr && !lookup->requiresADL() && lookup->decls_begin() != lookup->decls_end() &&
             std::ranges::all_of(
               lookup->decls(),
               [](NamedDecl const* declaration)
               {
                 declaration = declaration->getUnderlyingDecl();
                 auto const* functionTemplate = dyn_cast<FunctionTemplateDecl>(declaration);
                 auto const* function = functionTemplate == nullptr ? dyn_cast<FunctionDecl>(declaration)
                                                                    : functionTemplate->getTemplatedDecl();
                 return function != nullptr && boolSourceDeclaration(*function).getReturnType()->isBooleanType();
               });
    }

    bool hasOnlyBoolReturns(Stmt const* statement, bool& hasReturn)
    {
      if (statement == nullptr || isa<LambdaExpr>(statement))
      {
        // Lambda bodies belong to another function; Stmt children do not enter
        // local class method declarations either.
        return true;
      }

      if (auto const* returned = dyn_cast<ReturnStmt>(statement); returned != nullptr)
      {
        hasReturn = true;
        return hasSourceBoolExpression(returned->getRetValue());
      }

      // Inspect both source branches of if constexpr, not a selected instantiation.
      return std::ranges::all_of(
        statement->children(), [&hasReturn](Stmt const* child) { return hasOnlyBoolReturns(child, hasReturn); });
    }

    bool hasSourceBoolContract(FunctionDecl const& function)
    {
      if (function.getReturnType()->isBooleanType())
      {
        return true;
      }

      auto const* returnType = function.getReturnType()->getAs<AutoType>();
      FunctionDecl const* definition = function.getDefinition();

      if (returnType == nullptr || returnType->isDecltypeAuto() || definition == nullptr)
      {
        return false;
      }

      bool hasReturn = false;
      return hasOnlyBoolReturns(definition->getBody(), hasReturn) && hasReturn;
    }

    bool hasWordPrefix(StringRef name, StringRef prefix)
    {
      bool const matchesInitial =
        !name.empty() && (name.front() == prefix.front() || name.front() == toUppercase(prefix.front()));

      if (!matchesInitial || !name.drop_front().starts_with(prefix.drop_front()))
      {
        return false;
      }

      if (name.size() == prefix.size())
      {
        return true;
      }

      unsigned char const boundary = static_cast<unsigned char>(name[prefix.size()]);
      return name[prefix.size()] == '_' || isUppercase(boundary) || isDigit(boundary);
    }

    bool hasBoolFunctionVocabulary(StringRef name)
    {
      static constexpr auto kPrefixes = std::to_array<StringRef>({"is",
                                                                  "has",
                                                                  "can",
                                                                  "should",
                                                                  "supports",
                                                                  "needs",
                                                                  "matches",
                                                                  "accepts",
                                                                  "covers",
                                                                  "try",
                                                                  "contains",
                                                                  "startsWith",
                                                                  "endsWith"});

      return std::ranges::any_of(kPrefixes, [&name](StringRef prefix) { return hasWordPrefix(name, prefix); }) ||
             name == "empty" || name == "compare_exchange_strong" || name == "compare_exchange_weak" ||
             name == "asBool" || name == "readBoolOr";
    }
  } // namespace

  void BoolFunctionNamingCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(functionDecl().bind("function"), this);
  }

  void BoolFunctionNamingCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* function = result.Nodes.getNodeAs<FunctionDecl>("function");

    if (function == nullptr || result.SourceManager == nullptr)
    {
      return;
    }

    FunctionDecl const& sourceFunction = boolSourceDeclaration(*function);

    if (!function_naming::isProjectOwnedNamedFunction(sourceFunction, *result.SourceManager) ||
        function_naming::isFrameworkRequiredFunction(sourceFunction, *result.SourceManager) ||
        function_naming::isFrameworkRequiredFunction(*function, *result.SourceManager) ||
        hasBoolFunctionVocabulary(sourceFunction.getName()) || !hasSourceBoolContract(sourceFunction) ||
        !_diagnosedFunctions.insert(&sourceFunction).second)
    {
      return;
    }

    diag(sourceFunction.getLocation(),
         "source-fixed bool function '%0' must use predicate vocabulary, 'try*' for a success-reporting action, "
         "or supported bool conversion vocabulary")
      << sourceFunction.getName();
  }
} // namespace clang::tidy::readability
