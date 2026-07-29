// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang/Basic/SourceLocation.h>

#include <string>

namespace clang
{
  class ASTContext;
  class CallExpr;
  class CXXOperatorCallExpr;
  class Expr;
  class LangOptions;
  class SourceManager;
  class VarDecl;
} // namespace clang

namespace clang::tidy::aobus
{
  std::string getExprSourceText(Expr const& expr, SourceManager const& sm, LangOptions const& langOpts);

  // True when any end of the range sits inside a macro expansion; a FixIt
  // anchored there would edit the macro definition, not the use site.
  bool isInMacro(SourceRange range);

  // Unwraps the implicit wrapper chains the AST inserts around expressions
  // (ImplicitCastExpr / single-argument CXXConstructExpr /
  // MaterializeTemporaryExpr) to reach the expression as written.
  Expr const* stripImplicitNodes(Expr const* expr);

  // True when expr (modulo parens and implicit casts) is a reference to var.
  // Comparing declarations beats comparing source text: it is immune to
  // spelling differences and to same-named variables from other scopes.
  bool refersToVarDecl(Expr const* expr, VarDecl const& var);

  // A C++20 rewritten comparison (a != b lowered to !(a == b)) contains a
  // synthesized operator== call whose source-level operator is actually !=.
  // Matchers running in AsIs traversal see such inner nodes; checks must skip
  // them because the CXXRewrittenBinaryOperator itself is matched separately
  // with the correct operator name.
  bool isWithinRewrittenOperator(Expr const& expr, ASTContext& context);

  // Returns the unqualified name of the std::ranges function object invoked by
  // this operator() call, or an empty string when the callee is not a
  // std::ranges CPO. The qualified-name prefix check keeps this robust against
  // the implementation-detail inline namespaces CPOs live in.
  std::string getRangesCpoName(CXXOperatorCallExpr const& call);

  // Verifies via the AST (not source text) that endCall is an end()/cend()
  // call: a member call, a std::ranges::end/cend CPO invocation, or a free
  // std::end/std::cend call.
  bool isEndCall(CallExpr const& endCall);

  // Verifies that the object endCall is invoked on spells the same source text
  // as the range argument of the algorithm, rejecting cross-container
  // comparisons like find(v, x) != w.end().
  bool verifyEndObject(CallExpr const& endCall,
                       std::string const& rangeStr,
                       SourceManager const& sm,
                       LangOptions const& langOpts);
} // namespace clang::tidy::aobus
