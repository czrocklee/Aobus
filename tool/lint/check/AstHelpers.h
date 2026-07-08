// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringRef.h>

#include <string>

namespace clang::tidy::aobus
{
  inline std::string getExprSourceText(Expr const& expr, SourceManager const& sm, LangOptions const& langOpts)
  {
    return Lexer::getSourceText(CharSourceRange::getTokenRange(expr.getSourceRange()), sm, langOpts).str();
  }

  // True when any end of the range sits inside a macro expansion; a FixIt
  // anchored there would edit the macro definition, not the use site.
  inline bool isInMacro(SourceRange const range)
  {
    return range.getBegin().isInvalid() || range.getEnd().isInvalid() || range.getBegin().isMacroID() ||
           range.getEnd().isMacroID();
  }

  // Unwraps the implicit wrapper chains the AST inserts around expressions
  // (ImplicitCastExpr / single-argument CXXConstructExpr /
  // MaterializeTemporaryExpr) to reach the expression as written.
  inline Expr const* stripImplicitNodes(Expr const* expr)
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

  // True when expr (modulo parens and implicit casts) is a reference to var.
  // Comparing declarations beats comparing source text: it is immune to
  // spelling differences and to same-named variables from other scopes.
  inline bool refersToVarDecl(Expr const* expr, VarDecl const& var)
  {
    if (expr == nullptr)
    {
      return false;
    }

    auto const* declRef = dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts());

    return declRef != nullptr && declRef->getDecl()->getCanonicalDecl() == var.getCanonicalDecl();
  }

  // A C++20 rewritten comparison (a != b lowered to !(a == b)) contains a
  // synthesized operator== call whose source-level operator is actually !=.
  // Matchers running in AsIs traversal see such inner nodes; checks must skip
  // them because the CXXRewrittenBinaryOperator itself is matched separately
  // with the correct operator name.
  inline bool isWithinRewrittenOperator(Expr const& expr, ASTContext& context)
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

  // Returns the unqualified name of the std::ranges function object invoked by
  // this operator() call, or an empty string when the callee is not a
  // std::ranges CPO. The qualified-name prefix check keeps this robust against
  // the implementation-detail inline namespaces CPOs live in.
  inline std::string getRangesCpoName(CXXOperatorCallExpr const& call)
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

  // Verifies via the AST (not source text) that endCall is an end()/cend()
  // call: a member call, a std::ranges::end/cend CPO invocation, or a free
  // std::end/std::cend call.
  inline bool isEndCall(CallExpr const& endCall)
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

  // Verifies that the object endCall is invoked on spells the same source text
  // as the range argument of the algorithm, rejecting cross-container
  // comparisons like find(v, x) != w.end().
  inline bool verifyEndObject(CallExpr const& endCall,
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
