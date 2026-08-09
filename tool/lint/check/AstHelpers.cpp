// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringRef.h>

#include <string>
#include <string_view>

namespace clang::tidy::aobus
{
  std::string getExprSourceText(Expr const& expr, SourceManager const& sm, LangOptions const& langOpts)
  {
    return Lexer::getSourceText(CharSourceRange::getTokenRange(expr.getSourceRange()), sm, langOpts).str();
  }

  bool isInMacro(SourceRange const range)
  {
    return range.getBegin().isInvalid() || range.getEnd().isInvalid() || range.getBegin().isMacroID() ||
           range.getEnd().isMacroID();
  }

  bool isPolicySource(SourceManager const& sourceManager, SourceLocation const location)
  {
    if (location.isInvalid())
    {
      return false;
    }

    auto const spellingLocation = sourceManager.getSpellingLoc(location);

    if (sourceManager.isInSystemHeader(spellingLocation))
    {
      return false;
    }

    auto const filename = sourceManager.getFilename(spellingLocation);
    auto const isTestSource = filename.contains("/test/") || filename.contains("\\test\\");
    auto const isLintFixture =
      filename.contains("/test/integration/lint/fixture/") || filename.contains(R"(\test\integration\lint\fixture\)");
    return !isTestSource || isLintFixture;
  }

  bool enclosingFunctionBeginsWithPolicyMarker(Stmt const& statement,
                                               ASTContext& context,
                                               SourceManager const& sourceManager,
                                               std::string_view const markerHelperName,
                                               std::string_view const macroName)
  {
    auto current = DynTypedNode::create(statement);
    FunctionDecl const* function = nullptr;

    while (function == nullptr)
    {
      auto const parents = context.getParents(current);

      if (parents.empty())
      {
        return false;
      }

      auto const& parent = *parents.begin();
      function = parent.get<FunctionDecl>();
      current = parent;
    }

    auto const* body = function->getBody();
    return body != nullptr && blockBeginsWithPolicyMarker(*body, context, sourceManager, markerHelperName, macroName);
  }

  bool blockBeginsWithPolicyMarker(Stmt const& block,
                                   ASTContext const& context,
                                   SourceManager const& sourceManager,
                                   std::string_view const markerHelperName,
                                   std::string_view const macroName)
  {
    auto const* body = dyn_cast<CompoundStmt>(&block);

    if (body == nullptr || body->body_empty())
    {
      return false;
    }

    auto const* firstExpression = dyn_cast<Expr>(*body->body_begin());

    if (firstExpression == nullptr)
    {
      return false;
    }

    firstExpression = firstExpression->IgnoreParenImpCasts();

    if (auto const* cleanup = dyn_cast<ExprWithCleanups>(firstExpression); cleanup != nullptr)
    {
      firstExpression = cleanup->getSubExpr()->IgnoreParenImpCasts();
    }

    auto const* markerCall = dyn_cast<CallExpr>(firstExpression);
    auto const* markerFunction = markerCall != nullptr ? markerCall->getDirectCallee() : nullptr;

    if (markerFunction == nullptr || markerFunction->getQualifiedNameAsString() != markerHelperName)
    {
      return false;
    }

    auto const markerLocation = markerCall->getBeginLoc();

    if (!markerLocation.isMacroID())
    {
      return false;
    }

    auto const immediateMacro = Lexer::getImmediateMacroName(markerLocation, sourceManager, context.getLangOpts());
    return immediateMacro == llvm::StringRef{macroName.data(), macroName.size()};
  }

  Expr const* stripImplicitNodes(Expr const* expr)
  {
    if (expr == nullptr)
    {
      return nullptr;
    }

    Expr const* current = expr;

    while (true)
    {
      if (auto const* ice = dyn_cast<ImplicitCastExpr>(current); ice != nullptr)
      {
        current = ice->getSubExpr();
      }
      else if (auto const* cce = dyn_cast<CXXConstructExpr>(current); cce != nullptr)
      {
        if (cce->getNumArgs() == 1)
        {
          current = cce->getArg(0);
        }
        else
        {
          break;
        }
      }
      else if (auto const* mte = dyn_cast<MaterializeTemporaryExpr>(current); mte != nullptr)
      {
        current = mte->getSubExpr();
      }
      else
      {
        break;
      }
    }

    return current;
  }

  bool refersToVarDecl(Expr const* expr, VarDecl const& var)
  {
    if (expr == nullptr)
    {
      return false;
    }

    auto const* declRef = dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts());

    return declRef != nullptr && declRef->getDecl()->getCanonicalDecl() == var.getCanonicalDecl();
  }

  bool isWithinRewrittenOperator(Expr const& expr, ASTContext& context)
  {
    auto node = DynTypedNode::create(expr);

    while (true)
    {
      auto const parents = context.getParents(node);

      if (parents.empty())
      {
        return false;
      }

      auto const& parent = parents[0];

      if (parent.get<CXXRewrittenBinaryOperator>() != nullptr)
      {
        return true;
      }

      if (auto const* parentExpr = parent.get<Expr>();
          parentExpr == nullptr ||
          !(isa<UnaryOperator>(parentExpr) || isa<ParenExpr>(parentExpr) || isa<ImplicitCastExpr>(parentExpr)))
      {
        return false;
      }

      node = parent;
    }
  }

  std::string getRangesCpoName(CXXOperatorCallExpr const& call)
  {
    if (call.getNumArgs() == 0)
    {
      return {};
    }

    auto const* functorArgument = call.getArg(0)->IgnoreParenImpCasts();

    if (functorArgument->getType()->getAsCXXRecordDecl() == nullptr)
    {
      return {};
    }

    auto const* declRef = dyn_cast<DeclRefExpr>(functorArgument);

    if (declRef == nullptr)
    {
      return {};
    }

    auto const* decl = declRef->getFoundDecl();

    if (decl->getIdentifier() == nullptr ||
        !llvm::StringRef{decl->getQualifiedNameAsString()}.starts_with("std::ranges::"))
    {
      return {};
    }

    return decl->getName().str();
  }

  bool isEndCall(CallExpr const& endCall)
  {
    if (auto const* memberCall = dyn_cast<CXXMemberCallExpr>(&endCall); memberCall != nullptr)
    {
      auto const* method = memberCall->getMethodDecl();

      if (method == nullptr || method->getIdentifier() == nullptr)
      {
        return false;
      }

      StringRef const name = method->getName();

      return name == "end" || name == "cend";
    }

    if (auto const* opCall = dyn_cast<CXXOperatorCallExpr>(&endCall); opCall != nullptr)
    {
      auto const name = getRangesCpoName(*opCall);

      return name == "end" || name == "cend";
    }

    auto const* calleeDecl = endCall.getDirectCallee();

    if (calleeDecl == nullptr || calleeDecl->getIdentifier() == nullptr)
    {
      return false;
    }

    StringRef const name = calleeDecl->getName();

    return name == "end" || name == "cend";
  }

  bool verifyEndObject(CallExpr const& endCall,
                       std::string const& rangeStr,
                       SourceManager const& sm,
                       LangOptions const& langOpts)
  {
    Expr const* endObj = nullptr;

    if (auto const* memberCall = dyn_cast<CXXMemberCallExpr>(&endCall); memberCall != nullptr)
    {
      endObj = memberCall->getImplicitObjectArgument();
    }
    else if (auto const* opCall = dyn_cast<CXXOperatorCallExpr>(&endCall); opCall != nullptr)
    {
      if (opCall->getNumArgs() > 1)
      {
        endObj = opCall->getArg(1);
      }
    }
    else if (endCall.getNumArgs() > 0)
    {
      endObj = endCall.getArg(0);
    }

    if (endObj == nullptr)
    {
      return true;
    }

    return getExprSourceText(*endObj->IgnoreParenImpCasts(), sm, langOpts) == rangeStr;
  }
} // namespace clang::tidy::aobus
