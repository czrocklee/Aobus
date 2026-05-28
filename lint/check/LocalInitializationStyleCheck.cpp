// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/LocalInitializationStyleCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringSwitch.h>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isPrimitiveType(QualType type)
    {
      type = type.getCanonicalType();

      if (type->isPointerType() || type->isReferenceType())
      {
        return true;
      }
      // Note: We exclude isEnumeralType() so that enums (like std::byte)
      // follow the 'auto x = T{...}' rule for non-primitives.
      return type->isArithmeticType() || type->isAnyCharacterType() || type->isNullPtrType();
    }

    bool isContainerWithInitializerList(QualType type)
    {
      auto const* record = type.getCanonicalType()->getAsCXXRecordDecl();

      if (record == nullptr)
      {
        return false;
      }

      StringRef const name = record->getName();
      return llvm::StringSwitch<bool>{name}
        .Cases("vector", "deque", "list", "forward_list", true)
        .Cases("set", "multiset", "unordered_set", "unordered_multiset", true)
        .Cases("map", "multimap", "unordered_map", "unordered_multimap", true)
        .Cases("basic_string", "string", true)
        .Default(false);
    }

    bool isStringOrStringView(QualType type)
    {
      auto const* record = type.getCanonicalType()->getAsCXXRecordDecl();

      if (record == nullptr)
      {
        return false;
      }

      StringRef const name = record->getName();
      return name == "basic_string" || name == "string" || name == "basic_string_view" || name == "string_view";
    }

    bool hasStringLiteralArg(Expr const* init)
    {
      if (init == nullptr)
      {
        return false;
      }

      if (auto const* ctor = dyn_cast<CXXConstructExpr>(init->IgnoreImplicit()))
      {
        if (ctor->getNumArgs() > 0)
        {
          auto const* arg = ctor->getArg(0)->IgnoreImplicit();
          return isa<StringLiteral>(arg);
        }
      }

      return false;
    }
  } // namespace

  void LocalInitializationStyleCheck::registerMatchers(MatchFinder* finder)
  {
    // Match explicit type declarations that use CXXConstructExpr or InitListExpr.
    // We want to skip 'auto x = some_function()' which binds a CallExpr to the VarDecl.
    auto hasBadInit = anyOf(hasInitializer(ignoringImplicit(cxxConstructExpr().bind("init"))),
                            hasInitializer(ignoringImplicit(initListExpr().bind("init"))),
                            unless(hasInitializer(expr())));

    finder->addMatcher(varDecl(unless(parmVarDecl()),
                               unless(isImplicit()),
                               hasBadInit,
                               unless(hasType(isAnyCharacter())),
                               unless(hasType(qualType(anyOf(isInteger(), booleanType(), realFloatingPointType())))))
                         .bind("var"),
                       this);

    // Primitive initialization (Rule 3.4.5: primitives use assignment-style)
    finder->addMatcher(
      varDecl(unless(parmVarDecl()),
              hasInitializer(expr().bind("init")),
              hasType(qualType(anyOf(isInteger(), booleanType(), realFloatingPointType(), isAnyCharacter()))))
        .bind("var"),
      this);
  }

  namespace
  {
    bool isInsideForRangeOrLambda(DynTypedNode node, ASTContext* context)
    {
      for (;;)
      {
        auto parents = context->getParents(node);

        if (parents.empty())
        {
          break;
        }

        node = parents[0];

        if (node.get<CompoundStmt>() != nullptr)
        {
          break;
        }

        if ((node.get<CXXForRangeStmt>() != nullptr) || (node.get<LambdaExpr>() != nullptr))
        {
          return true;
        }

        if (node.get<TranslationUnitDecl>() != nullptr)
        {
          break;
        }
      }

      return false;
    }

    bool isEligibleLocalVar(VarDecl const* var, SourceManager const& sm, ASTContext* context)
    {
      if (var == nullptr || !var->hasLocalStorage() || var->isStaticLocal())
      {
        return false;
      }

      if (QualType const type = var->getType(); type->isUndeducedType() || type->isDependentType())
      {
        return false;
      }

      if (TypeSourceInfo const* tsi = var->getTypeSourceInfo())
      {
        if (tsi->getType()->getContainedAutoType() != nullptr)
        {
          return false;
        }
      }

      if (SourceLocation const loc = var->getLocation(); loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return false;
      }

      return !isInsideForRangeOrLambda(DynTypedNode::create(*var), context);
    }

    struct InitInfo final
    {
      bool isListInit{false};
      bool isEmptyInit{false};
      bool shouldSkip{false};
    };

    InitInfo analyzeInitialization(Expr const* init)
    {
      auto info = InitInfo{};

      if (init == nullptr)
      {
        info.isEmptyInit = true;
        return info;
      }

      if (auto const* ctorExpr = dyn_cast<CXXConstructExpr>(init->IgnoreImplicit()))
      {
        if (ctorExpr->getNumArgs() == 1 && ctorExpr->getArg(0)->getType()->isNullPtrType())
        {
          info.shouldSkip = true;
          return info;
        }

        if (auto const* ctorDecl = ctorExpr->getConstructor(); ctorDecl != nullptr)
        {
          if (auto const* record = ctorDecl->getParent(); record != nullptr && record->isLambda())
          {
            info.shouldSkip = true;
            return info;
          }
        }

        info.isListInit = ctorExpr->isListInitialization();
        info.isEmptyInit = ctorExpr->getNumArgs() == 0;
      }
      else if (isa<InitListExpr>(init->IgnoreImplicit()))
      {
        info.isListInit = true;
      }

      return info;
    }
  }

  void LocalInitializationStyleCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* var = result.Nodes.getNodeAs<VarDecl>("var");
    auto const* init = result.Nodes.getNodeAs<Expr>("init");

    if (!isEligibleLocalVar(var, sm, result.Context))
    {
      return;
    }

    SourceLocation const loc = var->getLocation();
    QualType const varType = var->getType();

    if (bool const primitive = isPrimitiveType(varType); !primitive)
    {
      auto const info = analyzeInitialization(init);

      if (info.shouldSkip)
      {
        return;
      }

      bool const isContainer = isContainerWithInitializerList(varType);
      bool const isString = isStringOrStringView(varType);

      if (bool const hasLiteral = hasStringLiteralArg(init); isString && hasLiteral)
      {
        auto const suffix = StringRef{varType.getAsString().find("string_view") != StringRef::npos ? "sv" : "s"};
        diag(loc, "prefer standard literals 'auto %0 = \"...\"%1' over explicit string construction")
          << var->getName() << suffix;
      }
      else if (isContainer && !info.isListInit && !info.isEmptyInit)
      {
        diag(loc, "use 'auto %0 = Type(...)' for container initialization to avoid ambiguity") << var->getName();
      }
      else
      {
        diag(loc, "use 'auto %0 = Type{...}' instead of explicit type initialization") << var->getName();
      }
    }
    else if (var->getInitStyle() != VarDecl::CInit)
    {
      diag(loc, "primitive type should use assignment-style initialization 'Type %0 = ...'") << var->getName();
    }
  }
} // namespace clang::tidy::readability
