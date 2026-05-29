// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseIfInitStatementCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Lex/Lexer.h>

#include <algorithm>
#include <iterator>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    class UsageVisitor : public RecursiveASTVisitor<UsageVisitor>
    {
    public:
      UsageVisitor(VarDecl const* varDecl)
        : _varDecl{varDecl}
      {
      }

      bool VisitDeclRefExpr(DeclRefExpr* declRefExpr)
      {
        if (declRefExpr->getDecl() == _varDecl)
        {
          _found = true;
          return false; // Stop traversal
        }

        return true;
      }

      bool found() const { return _found; }

    private:
      VarDecl const* _varDecl;
      bool _found{false};
    };

    bool isUsedInside(VarDecl const* varDecl, Stmt const* target)
    {
      auto visitor = UsageVisitor{varDecl};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      visitor.TraverseStmt(const_cast<Stmt*>(target));
      return visitor.found();
    }

    bool isUsedAfter(VarDecl const* varDecl, CompoundStmt const* block, Stmt const* target)
    {
      auto const body = block->body();
      auto const it = std::ranges::find(body, target);

      if (it == body.end())
      {
        return false;
      }

      for (auto const* nextIt = std::next(it); nextIt != body.end(); ++nextIt)
      {
        auto visitor = UsageVisitor{varDecl};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        visitor.TraverseStmt(const_cast<Stmt*>(*nextIt));

        if (visitor.found())
        {
          return true;
        }
      }

      return false;
    }

    bool isEligibleVarDecl(VarDecl const* varDecl, DeclStmt const* decl, SourceManager const& sm)
    {
      if ((varDecl == nullptr) || !varDecl->hasInit())
      {
        return false;
      }

      // Rule: Ignore constexpr or static variables (typically config constants)
      if (varDecl->isConstexpr() || varDecl->getStorageClass() == SC_Static)
      {
        return false;
      }

      // Rule: Ignore multiline declarations or very long ones to keep it "reasonable"
      auto const declRange = decl->getSourceRange();

      return sm.getSpellingLineNumber(declRange.getBegin()) == sm.getSpellingLineNumber(declRange.getEnd());
    }

    bool isEligibleControlStmt(Stmt const* target)
    {
      if (auto const* ifStmt = dyn_cast<IfStmt>(target); ifStmt)
      {
        // If it already has an init statement OR a condition variable, don't crowd it.
        return !ifStmt->hasInitStorage() && (ifStmt->getConditionVariable() == nullptr);
      }

      if (auto const* switchStmt = dyn_cast<SwitchStmt>(target); switchStmt)
      {
        return !switchStmt->hasInitStorage() && (switchStmt->getConditionVariable() == nullptr);
      }

      return false;
    }
  } // namespace

  void UseIfInitStatementCheck::registerMatchers(MatchFinder* finder)
  {
    // Matcher 1: Variable declared before control statement
    finder->addMatcher(declStmt(hasParent(compoundStmt().bind("block"))).bind("decl"), this);

    // Matcher 2: Implicit condition variable inside if
    finder->addMatcher(ifStmt(hasConditionVariableStatement(declStmt(hasSingleDecl(varDecl().bind("condVar"))).bind("condDecl")),
                              unless(hasInitStatement(anything())))
                         .bind("implicitIf"),
                       this);
  }

  void UseIfInitStatementCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;

    // Handle Matcher 2: Implicit condition variable
    if (auto const* implicitIf = result.Nodes.getNodeAs<IfStmt>("implicitIf"))
    {
      auto const* condVar = result.Nodes.getNodeAs<VarDecl>("condVar");
      auto const* condDecl = result.Nodes.getNodeAs<DeclStmt>("condDecl");

      if ((implicitIf == nullptr) || (condVar == nullptr) || (condDecl == nullptr))
      {
        return;
      }

      if (sm.isInSystemHeader(condVar->getBeginLoc()) || condVar->getBeginLoc().isMacroID())
      {
        return;
      }

      diag(condVar->getBeginLoc(), "prefer explicit init-statement style: if (auto var = expr; var)");
      return;
    }

    // Handle Matcher 1: Variable declared before control statement
    auto const* block = result.Nodes.getNodeAs<CompoundStmt>("block");
    auto const* decl = result.Nodes.getNodeAs<DeclStmt>("decl");

    if ((block == nullptr) || (decl == nullptr))
    {
      return;
    }

    if (sm.isInSystemHeader(decl->getBeginLoc()) || decl->getBeginLoc().isMacroID())
    {
      return;
    }

    // Ensure DeclStmt has exactly one variable declaration
    if (!decl->isSingleDecl())
    {
      return;
    }

    auto const* varDecl = dyn_cast<VarDecl>(decl->getSingleDecl());

    if (!isEligibleVarDecl(varDecl, decl, sm))
    {
      return;
    }

    // Find the next statement in the block
    auto const body = block->body();
    auto const it = std::ranges::find(body, decl);
    auto const* target = (it != body.end() && std::next(it) != body.end()) ? *std::next(it) : nullptr;

    if ((target == nullptr) || !isEligibleControlStmt(target))
    {
      return;
    }

    // Ensure the variable is actually used INSIDE the target statement.
    // This prevents swallowing RAII locks that just happen to precede the statement.
    if (!isUsedInside(varDecl, target))
    {
      return;
    }

    // Check if variable is used after the target statement
    if (isUsedAfter(varDecl, block, target))
    {
      return;
    }

    // Get declaration text
    auto const declText = Lexer::getSourceText(
      CharSourceRange::getTokenRange(varDecl->getBeginLoc(), varDecl->getEndLoc()), sm, result.Context->getLangOpts());

    // Rule: If the declaration itself is too long, don't merge it.
    if (declText.size() > 60)
    {
      return;
    }

    // Everything looks good, issue a warning
    auto varName = std::string{};

    if (isa<DecompositionDecl>(varDecl))
    {
      varName = "[...]"; // Structured bindings
    }
    else
    {
      varName = "'" + varDecl->getName().str() + "'";
    }

    bool const isIf = isa<IfStmt>(target);
    auto diagBuilder = diag(decl->getBeginLoc(),
                            "variable %0 is only used inside the following '%1' "
                            "statement; move its declaration into the init-statement")
                       << varName << (isIf ? "if" : "switch");

    // Fix-it logic
    auto const stmtBegin = decl->getBeginLoc();
    auto const stmtEnd = decl->getEndLoc();
    auto const optNextTok = Lexer::findNextToken(stmtEnd, sm, result.Context->getLangOpts());
    auto removalEnd = SourceLocation{};

    if (optNextTok && optNextTok->is(tok::semi))
    {
      removalEnd = optNextTok->getEndLoc();
    }
    else
    {
      removalEnd = Lexer::getLocForEndOfToken(stmtEnd, 0, sm, result.Context->getLangOpts());
    }

    // Remove the declaration statement
    diagBuilder << FixItHint::CreateRemoval(SourceRange{stmtBegin, removalEnd});

    // Insert declaration into target
    auto const insertLoc = isIf ? cast<IfStmt>(target)->getLParenLoc().getLocWithOffset(1)
                                : cast<SwitchStmt>(target)->getLParenLoc().getLocWithOffset(1);

    if (insertLoc.isInvalid())
    {
      return;
    }

    // Insert the previously fetched declaration text
    diagBuilder << FixItHint::CreateInsertion(insertLoc, declText.str() + "; ");
  }
} // namespace clang::tidy::readability
