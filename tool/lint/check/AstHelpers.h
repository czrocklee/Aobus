// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyOptions.h>
#include <clang/Basic/SourceLocation.h>

#include <string>
#include <string_view>
#include <vector>

namespace clang
{
  class ASTContext;
  class CallExpr;
  class CXXOperatorCallExpr;
  class Expr;
  class LangOptions;
  class SourceManager;
  class Stmt;
  class VarDecl;
} // namespace clang

namespace clang::tidy::aobus
{
  std::string getExprSourceText(Expr const& expr, SourceManager const& sm, LangOptions const& langOpts);

  // True when any end of the range sits inside a macro expansion; a location
  // anchored there points into the macro definition, not the use site, so
  // covered checks skip such ranges before diagnosing.
  bool isInMacro(SourceRange range);

  // Parse the portal's validated source-root option once per check instance.
  // Root-level ordinary tests and system headers are outside production policy;
  // lint fixtures remain covered. Unmatched paths never gain a test exemption.
  // Each fresh ClangTidy check/TU owns one scope for a single SourceManager
  // epoch with immutable roots and filename resolution; do not reuse it across
  // managers, reparses or cleared source IDs. Only path classification is cached.
  class PolicySourceScope final
  {
  public:
    PolicySourceScope(ClangTidyCheck const& check, ClangTidyCheck::OptionsView const& options);

    void storeOptions(ClangTidyCheck::OptionsView const& view, ClangTidyOptions::OptionMap& options) const;
    bool isPolicySource(SourceManager const& sourceManager, SourceLocation location) const;

  private:
    struct Root final
    {
      std::string path;
      bool isWindowsPath;
    };

    std::string _sourceRootsJson;
    std::vector<Root> _roots;
    mutable FileID _cachedFileId;
    mutable bool _cachedFileIsPolicySource = true;
  };

  bool hasBlockPolicyMarker(Stmt const& block,
                            ASTContext const& context,
                            SourceManager const& sourceManager,
                            std::string_view markerHelperName,
                            std::string_view macroName);

  // True when the function enclosing statement begins with the exact policy
  // marker macro expansion backed by markerHelperName.
  bool hasEnclosingFunctionPolicyMarker(Stmt const& statement,
                                        ASTContext& context,
                                        SourceManager const& sourceManager,
                                        std::string_view markerHelperName,
                                        std::string_view macroName);

  // Unwraps the implicit wrapper chains the AST inserts around expressions
  // (ImplicitCastExpr / single-argument CXXConstructExpr /
  // MaterializeTemporaryExpr) to reach the expression as written.
  Expr const* stripImplicitNodes(Expr const* expr);

  // True when expr (modulo parens and implicit casts) is a reference to var.
  // Comparing declarations beats comparing source text: it is immune to
  // spelling differences and to same-named variables from other scopes.
  bool isVarDeclReference(Expr const* expr, VarDecl const& var);

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
  bool isMatchingEndObject(CallExpr const& endCall,
                           std::string const& rangeStr,
                           SourceManager const& sm,
                           LangOptions const& langOpts);
} // namespace clang::tidy::aobus
