// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "UseRangesProjectionCheck.h"

#include "AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Lex/Lexer.h>

#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  void UseRangesProjectionCheck::registerMatchers(MatchFinder* finder)
  {
    if (!getLangOpts().CPlusPlus20)
    {
      return;
    }

    auto parameter1 = parmVarDecl().bind("parameter1");
    auto singleArgLambda =
      lambdaExpr(
        has(cxxRecordDecl(has(cxxMethodDecl(
          parameterCountIs(1),
          hasParameter(0, parameter1),
          hasBody(compoundStmt(
            statementCountIs(1),
            hasAnySubstatement(returnStmt(hasReturnValue(ignoringParenImpCasts(anyOf(
              cxxMemberCallExpr(callee(cxxMethodDecl().bind("method")),
                                on(ignoringParenImpCasts(declRefExpr(to(parmVarDecl(equalsBoundNode("parameter1")))))),
                                argumentCountIs(0))
                .bind("member_call"),
              memberExpr(
                hasObjectExpression(ignoringParenImpCasts(declRefExpr(to(parmVarDecl(equalsBoundNode("parameter1")))))))
                .bind("member_access")))))))))))))
        .bind("single_lambda");

    auto parameterA = parmVarDecl().bind("parameterA");
    auto parameterB = parmVarDecl().bind("parameterB");
    auto doubleArgLambda =
      lambdaExpr(has(cxxRecordDecl(has(cxxMethodDecl(
                   parameterCountIs(2),
                   hasParameter(0, parameterA),
                   hasParameter(1, parameterB),
                   hasBody(compoundStmt(
                     statementCountIs(1),
                     hasAnySubstatement(returnStmt(hasReturnValue(ignoringParenImpCasts(
                       binaryOperation(
                         hasOperatorName("<"),
                         hasLHS(ignoringParenImpCasts(memberExpr(hasObjectExpression(ignoringParenImpCasts(declRefExpr(
                                                                   to(parmVarDecl(equalsBoundNode("parameterA")))))))
                                                        .bind("memA"))),
                         hasRHS(ignoringParenImpCasts(memberExpr(hasObjectExpression(ignoringParenImpCasts(declRefExpr(
                                                                   to(parmVarDecl(equalsBoundNode("parameterB")))))))
                                                        .bind("memB"))))
                         .bind("less_than"))))))))))))
        .bind("double_lambda");

    finder->addMatcher(singleArgLambda, this);
    finder->addMatcher(doubleArgLambda, this);
  }

  void UseRangesProjectionCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* singleLambda = result.Nodes.getNodeAs<LambdaExpr>("single_lambda");
    auto const* doubleLambda = result.Nodes.getNodeAs<LambdaExpr>("double_lambda");

    auto policy = PrintingPolicy{result.Context->getPrintingPolicy()};
    policy.SuppressTagKeyword = true;

    if (singleLambda != nullptr)
    {
      auto const* parameter1 = result.Nodes.getNodeAs<ParmVarDecl>("parameter1");
      auto const* methodCall = result.Nodes.getNodeAs<CXXMemberCallExpr>("member_call");
      auto const* memberAccess = result.Nodes.getNodeAs<MemberExpr>("member_access");

      if (parameter1 == nullptr)
      {
        return;
      }

      // A dependent parameter type (generic lambda) cannot be spelled in a
      // &Type::member projection.
      if (parameter1->getType().getNonReferenceType()->isDependentType() ||
          aobus::isInMacro(singleLambda->getSourceRange()))
      {
        return;
      }

      auto const typeName = parameter1->getType().getNonReferenceType().getUnqualifiedType().getAsString(policy);
      auto memberName = std::string{};

      if (methodCall != nullptr)
      {
        auto const* method = result.Nodes.getNodeAs<CXXMethodDecl>("method");

        if (method == nullptr)
        {
          return;
        }

        memberName = method->getNameAsString();
      }
      else if (memberAccess != nullptr)
      {
        memberName = memberAccess->getMemberDecl()->getNameAsString();
      }
      else
      {
        return;
      }

      auto const replacement = "&" + typeName + "::" + memberName;

      diag(singleLambda->getBeginLoc(), "use a projection instead of a single-argument lambda")
        << FixItHint::CreateReplacement(singleLambda->getSourceRange(), replacement);
    }
    else if (doubleLambda != nullptr)
    {
      auto const* parameterA = result.Nodes.getNodeAs<ParmVarDecl>("parameterA");
      auto const* memA = result.Nodes.getNodeAs<MemberExpr>("memA");
      auto const* memB = result.Nodes.getNodeAs<MemberExpr>("memB");

      if (parameterA == nullptr || memA == nullptr || memB == nullptr)
      {
        return;
      }

      if (parameterA->getType().getNonReferenceType()->isDependentType() ||
          aobus::isInMacro(doubleLambda->getSourceRange()))
      {
        return;
      }

      auto const typeName = parameterA->getType().getNonReferenceType().getUnqualifiedType().getAsString(policy);
      auto const memAName = memA->getMemberDecl()->getNameAsString();
      auto const memBName = memB->getMemberDecl()->getNameAsString();

      if (memAName != memBName)
      {
        return;
      }

      auto const replacement = "{}, &" + typeName + "::" + memAName;

      diag(doubleLambda->getBeginLoc(), "use std::ranges::less with a projection instead of a double-argument lambda")
        << FixItHint::CreateReplacement(doubleLambda->getSourceRange(), replacement);
    }
  }
} // namespace clang::tidy::readability
