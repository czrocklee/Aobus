// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseStdToArrayCheck.h"

#include <clang/AST/Decl.h>
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

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::modernize
{
  namespace
  {
    bool isToArrayCall(Expr const* const expr)
    {
      if (expr == nullptr)
      {
        return false;
      }

      auto const* const clean = expr->IgnoreImplicit();

      if (auto const* const call = dyn_cast<CallExpr>(clean); call != nullptr)
      {
        if (auto const* const callee = call->getDirectCallee(); callee != nullptr)
        {
          return callee->getNameAsString() == "to_array";
        }
      }

      return false;
    }

    InitListExpr const* getInitList(Expr const* const expr)
    {
      if (expr == nullptr)
      {
        return nullptr;
      }

      auto const* const clean = expr->IgnoreImplicit();

      if (auto const* const initList = dyn_cast<InitListExpr>(clean); initList != nullptr)
      {
        return initList;
      }

      if (auto const* const construct = dyn_cast<CXXConstructExpr>(clean); construct != nullptr)
      {
        for (auto const* const argument : construct->arguments())
        {
          if (argument == nullptr)
          {
            continue;
          }

          if (auto const* const sub = getInitList(argument); sub != nullptr)
          {
            return sub;
          }
        }
      }

      return nullptr;
    }

    bool hasExplicitTemplateArgs(TypeLoc const cleanTypeLoc)
    {
      auto const tsLoc = cleanTypeLoc.getAs<TemplateSpecializationTypeLoc>();

      return !tsLoc.isNull() && tsLoc.getLAngleLoc().isValid();
    }

    bool hasRedundantElementType(InitListExpr const* const targetList)
    {
      for (auto const* const init : targetList->inits())
      {
        if (init == nullptr)
        {
          continue;
        }

        auto const* const clean = init->IgnoreImplicit();
        auto typeRangeToRemove = SourceRange{};

        if (auto const* const tempObj = dyn_cast<CXXTemporaryObjectExpr>(clean); tempObj != nullptr)
        {
          if (tempObj->getTypeSourceInfo() != nullptr)
          {
            typeRangeToRemove = tempObj->getTypeSourceInfo()->getTypeLoc().getSourceRange();
          }
        }
        else if (auto const* const funcCast = dyn_cast<CXXFunctionalCastExpr>(clean); funcCast != nullptr)
        {
          if (funcCast->getTypeInfoAsWritten() != nullptr)
          {
            typeRangeToRemove = funcCast->getTypeInfoAsWritten()->getTypeLoc().getSourceRange();
          }
        }

        if (typeRangeToRemove.isValid())
        {
          return true;
        }
      }

      return false;
    }
  } // namespace

  void UseStdToArrayCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(varDecl(unless(isExpansionInSystemHeader()),
                               unless(isImplicit()),
                               unless(parmVarDecl()),
                               hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(
                                 classTemplateSpecializationDecl(hasName("std::array")).bind("specDecl"))))),
                               hasInitializer(expr().bind("initExpr")))
                         .bind("varDecl"),
                       this);
  }

  void UseStdToArrayCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* const varDecl = result.Nodes.getNodeAs<VarDecl>("varDecl");

    if (varDecl == nullptr)
    {
      return;
    }

    auto const* const initExpr = result.Nodes.getNodeAs<Expr>("initExpr");

    if (initExpr == nullptr || isToArrayCall(initExpr))
    {
      return;
    }

    auto const* const specDecl = result.Nodes.getNodeAs<ClassTemplateSpecializationDecl>("specDecl");

    if (specDecl == nullptr)
    {
      return;
    }

    auto const& sm = *result.SourceManager;
    auto const loc = varDecl->getBeginLoc();

    if (loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
    {
      return;
    }

    // Get template args check
    if (specDecl->getTemplateArgs().size() == 0)
    {
      return;
    }

    // Find the init list
    auto const* const outerList = getInitList(initExpr);

    if (outerList == nullptr)
    {
      return;
    }

    // Get the syntactic form of the initializer list
    auto const* syntaxOuter = outerList;

    if (outerList->isSemanticForm() && outerList->getSyntacticForm() != nullptr)
    {
      syntaxOuter = outerList->getSyntacticForm();
    }

    if (syntaxOuter == nullptr || syntaxOuter->getNumInits() == 0)
    {
      return;
    }

    // Check for explicit template arguments (like std::array<T, N>)
    auto const typeLoc = varDecl->getTypeSourceInfo()->getTypeLoc();
    bool const hasExplicitArgs = hasExplicitTemplateArgs(typeLoc.getUnqualifiedLoc());

    // Check if we have double braces syntactically
    auto const* const syntaxInner =
      (syntaxOuter->getNumInits() == 1) ? dyn_cast<InitListExpr>(syntaxOuter->getInit(0)->IgnoreImplicit()) : nullptr;

    auto const* const targetList = syntaxInner != nullptr ? syntaxInner : syntaxOuter;
    bool const hasTypeRemoval = hasRedundantElementType(targetList);

    // Only warn if:
    // 1. Spelled with explicit template arguments (e.g. std::array<T, N>)
    // 2. OR contains explicit type name inside the initializer elements (e.g. std::array codecs{ CodecName{...} })
    if (!hasExplicitArgs && !hasTypeRemoval)
    {
      return;
    }

    // Diagnostics builder
    diag(loc, "use std::to_array instead of std::array for array creation");
  }
} // namespace clang::tidy::modernize
