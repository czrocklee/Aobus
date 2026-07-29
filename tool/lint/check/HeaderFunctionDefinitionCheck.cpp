// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/HeaderFunctionDefinitionCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Specifiers.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Path.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isHeaderMainFile(SourceManager const& sourceManager)
    {
      auto const mainLocation = sourceManager.getLocForStartOfFile(sourceManager.getMainFileID());
      auto const extension = llvm::sys::path::extension(sourceManager.getFilename(mainLocation));
      return extension.equals_insensitive(".h") || extension.equals_insensitive(".hh") ||
             extension.equals_insensitive(".hpp") || extension.equals_insensitive(".hxx");
    }

    bool isLambdaCallOperator(FunctionDecl const& function)
    {
      auto const* method = llvm::dyn_cast<CXXMethodDecl>(&function);
      return method != nullptr && method->getParent()->isLambda();
    }

    bool requiresVisibleDefinition(FunctionDecl const& function)
    {
      if (function.isConstexpr() || function.getReturnType()->getContainedAutoType() != nullptr)
      {
        return true;
      }

      if (auto const templatedKind = function.getTemplatedKind(); templatedKind == FunctionDecl::TK_NonTemplate)
      {
        return function.getDeclContext()->isDependentContext();
      }

      if (function.getTemplateSpecializationKind() == TSK_ExplicitSpecialization)
      {
        return false;
      }

      return true;
    }

    class NestedExecutableDefinitionVisitor final : public RecursiveASTVisitor<NestedExecutableDefinitionVisitor>
    {
    public:
      bool VisitLambdaExpr(LambdaExpr const* /*expression*/)
      {
        _found = true;
        return false;
      }

      bool VisitStmtExpr(StmtExpr const* /*expression*/)
      {
        _found = true;
        return false;
      }

      bool VisitFunctionDecl(FunctionDecl const* function)
      {
        if (function->doesThisDeclarationHaveABody())
        {
          _found = true;
          return false;
        }

        return true;
      }

      bool found() const { return _found; }

    private:
      bool _found = false;
    };

    bool isSimpleStatement(Stmt& statement)
    {
      if (!llvm::isa<ReturnStmt, CoreturnStmt, Expr, DeclStmt>(statement))
      {
        return false;
      }

      auto visitor = NestedExecutableDefinitionVisitor{};
      visitor.TraverseStmt(&statement);
      return !visitor.found();
    }

    bool hasAllowedBody(FunctionDecl const& function)
    {
      auto const* body = function.getBody();
      auto const* compound = llvm::dyn_cast_or_null<CompoundStmt>(body);

      if (compound == nullptr)
      {
        return false;
      }

      if (compound->body_empty())
      {
        return true;
      }

      return compound->size() == 1 && isSimpleStatement(**compound->body_begin());
    }
  } // namespace

  void HeaderFunctionDefinitionCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(functionDecl(isDefinition(), unless(isExpansionInSystemHeader())).bind("function"), this);
  }

  void HeaderFunctionDefinitionCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const* function = result.Nodes.getNodeAs<FunctionDecl>("function");

    if (function == nullptr || function->isImplicit() || function->isDeleted() || function->isDefaulted() ||
        isLambdaCallOperator(*function) || requiresVisibleDefinition(*function) || hasAllowedBody(*function))
    {
      return;
    }

    auto const& sourceManager = *result.SourceManager;
    auto const location = sourceManager.getExpansionLoc(function->getLocation());

    if (location.isInvalid() || !sourceManager.isWrittenInMainFile(location) || !isHeaderMainFile(sourceManager))
    {
      return;
    }

    diag(location,
         "move non-trivial function definition %0 out of the header; ordinary header definitions may contain only "
         "an empty body or one simple statement")
      << function;
  }
} // namespace clang::tidy::readability
