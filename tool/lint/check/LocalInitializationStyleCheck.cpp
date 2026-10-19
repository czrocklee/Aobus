// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/LocalInitializationStyleCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Type.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringSwitch.h>

using clang::ast_matchers::MatchFinder;

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

    CXXRecordDecl const* getSourceKnownRecordDecl(QualType type)
    {
      type = type.getCanonicalType();

      if (auto const* record = type->getAsCXXRecordDecl(); record != nullptr)
      {
        return record;
      }

      // Canonicalization resolves safe class aliases but not aliases of an unknown category.
      if (auto const* specialization = type->getAs<TemplateSpecializationType>(); specialization != nullptr)
      {
        auto const* classTemplate =
          dyn_cast_or_null<ClassTemplateDecl>(specialization->getTemplateName().getAsTemplateDecl());
        return classTemplate == nullptr ? nullptr : classTemplate->getTemplatedDecl();
      }

      return nullptr;
    }

    bool isContainerWithInitializerList(QualType type)
    {
      auto const* record = getSourceKnownRecordDecl(type);

      if (record == nullptr)
      {
        return false;
      }

      StringRef const name = record->getName();
      return llvm::StringSwitch<bool>{name}
        .Cases({"vector", "deque", "list", "forward_list"}, true)
        .Cases({"set", "multiset", "unordered_set", "unordered_multiset"}, true)
        .Cases({"map", "multimap", "unordered_map", "unordered_multimap"}, true)
        .Cases({"basic_string", "string"}, true)
        .Default(false);
    }

    bool isStringOrStringView(QualType type)
    {
      auto const* record = getSourceKnownRecordDecl(type);

      if (record == nullptr)
      {
        return false;
      }

      StringRef const name = record->getName();
      return name == "basic_string" || name == "string" || name == "basic_string_view" || name == "string_view";
    }

    bool hasStringLiteralArgument(Expr const* init)
    {
      if (init == nullptr)
      {
        return false;
      }

      if (auto const* ctor = dyn_cast<CXXConstructExpr>(init->IgnoreImplicit()); ctor != nullptr)
      {
        if (ctor->getNumArgs() > 0)
        {
          auto const* argument = ctor->getArg(0)->IgnoreImplicit();
          return isa<StringLiteral>(argument);
        }
      }

      return false;
    }

    bool hasAutoTypeSpelling(VarDecl const* var)
    {
      auto const* typeInfo = var->getTypeSourceInfo();
      return typeInfo != nullptr && !typeInfo->getTypeLoc().getContainedAutoTypeLoc().isNull();
    }

    bool isPrimitiveFunctionalCastInitializer(Expr const* init)
    {
      auto const* clean = init == nullptr ? nullptr : init->IgnoreParenImpCasts();

      if (auto const* functionalCast = dyn_cast_or_null<CXXFunctionalCastExpr>(clean); functionalCast != nullptr)
      {
        auto const* typeInfo = functionalCast->getTypeInfoAsWritten();
        return typeInfo != nullptr && typeInfo->getType()->isArithmeticType();
      }

      // A dependent argument can leave a source-fixed arithmetic conversion unresolved.
      if (auto const* unresolved = dyn_cast_or_null<CXXUnresolvedConstructExpr>(clean); unresolved != nullptr)
      {
        return unresolved->getTypeAsWritten()->isArithmeticType();
      }

      return false;
    }

    bool isAllowedPrimitiveAutoInitializer(Expr const* init)
    {
      return isa_and_nonnull<CXXStaticCastExpr>(init == nullptr ? nullptr : init->IgnoreParenImpCasts());
    }

    bool isPrimitiveLiteralInitializer(Expr const* init)
    {
      auto const* clean = init == nullptr ? nullptr : init->IgnoreParenImpCasts();

      if (clean == nullptr)
      {
        return false;
      }

      if (auto const* unary = dyn_cast<UnaryOperator>(clean); unary != nullptr)
      {
        if (unary->getOpcode() == UO_Plus || unary->getOpcode() == UO_Minus)
        {
          return isPrimitiveLiteralInitializer(unary->getSubExpr());
        }
      }

      return isa<IntegerLiteral, FloatingLiteral, CXXBoolLiteralExpr, CharacterLiteral>(clean);
    }

    bool shouldDiagnosePrimitiveAutoInitializer(Expr const* init)
    {
      auto const* expression = init == nullptr ? nullptr : init->IgnoreParenImpCasts();

      // Direct-list auto deduction wraps the whole initializer in a one-element list.
      if (auto const* list = dyn_cast_or_null<InitListExpr>(expression); list != nullptr && list->getNumInits() == 1)
      {
        expression = list->getInit(0);
      }

      return isPrimitiveFunctionalCastInitializer(expression) || isPrimitiveLiteralInitializer(expression);
    }
  } // namespace

  void LocalInitializationStyleCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    // Include source parentheses for dependent records before constructors can resolve.
    // We want to skip 'auto x = some_function()' which binds a CallExpr to the VarDecl.
    auto hasBadInit = anyOf(hasInitializer(ignoringImplicit(cxxConstructExpr().bind("init"))),
                            hasInitializer(ignoringImplicit(initListExpr().bind("init"))),
                            hasInitializer(ignoringImplicit(parenListExpr().bind("init"))),
                            unless(hasInitializer(expr())));

    // Diagnose source patterns only: a dependent T{1} must not acquire a
    // primitive-initialization rule from one of its instantiations.
    finder->addMatcher(varDecl(unless(parmVarDecl()),
                               unless(isImplicit()),
                               unless(isInstantiated()),
                               hasBadInit,
                               unless(hasType(isAnyCharacter())),
                               unless(hasType(qualType(anyOf(isInteger(), booleanType(), realFloatingPointType())))))
                         .bind("var"),
                       this);

    // Primitive initialization uses assignment style.
    // unless(isImplicit()) skips compiler-synthesized VarDecls such as the
    // coroutine parameter-move copies, whose location lands on the first
    // coroutine keyword of the body and which have no stylable spelling.
    finder->addMatcher(
      varDecl(unless(parmVarDecl()),
              unless(isImplicit()),
              unless(isInstantiated()),
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

      if (QualType const type = var->getType();
          type->isUndeducedType() || (type->isDependentType() && getSourceKnownRecordDecl(type) == nullptr))
      {
        return false;
      }

      if (SourceLocation const loc = var->getLocation(); loc.isInvalid() || loc.isMacroID() || sm.isInSystemHeader(loc))
      {
        return false;
      }

      return !isInsideForRangeOrLambda(DynTypedNode::create(*var), context);
    }

    // A namespace-scope variable, static data member, or static local. Only the
    // primitive rules apply to it; non-primitive style remains local-only.
    bool isEligibleStaticVar(VarDecl const* var, SourceManager const& sm)
    {
      if (var == nullptr || var->hasLocalStorage() || !isPrimitiveType(var->getType()))
      {
        return false;
      }

      if (QualType const type = var->getType(); type->isUndeducedType() || type->isDependentType())
      {
        return false;
      }

      SourceLocation const loc = var->getLocation();
      return loc.isValid() && !loc.isMacroID() && !sm.isInSystemHeader(loc);
    }

    struct InitInfo final
    {
      bool isListInit{false};
      bool isEmptyInit{false};
      bool shouldSkip{false};
    };

    InitInfo analyzeInitialization(Expr const* init, QualType type)
    {
      auto info = InitInfo{};

      if (init == nullptr)
      {
        info.isEmptyInit = true;
        return info;
      }

      if (auto const* ctorExpr = dyn_cast<CXXConstructExpr>(init->IgnoreImplicit()); ctorExpr != nullptr)
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
      else if (auto const* list = dyn_cast<InitListExpr>(init->IgnoreImplicit()); list != nullptr)
      {
        info.isListInit = true;
        info.isEmptyInit = list->getNumInits() == 0;
        info.shouldSkip =
          type->isDependentType() && list->getNumInits() == 1 && list->getInit(0)->getType()->isNullPtrType();
      }
      else if (auto const* parens = dyn_cast<ParenListExpr>(init->IgnoreImplicit()); parens != nullptr)
      {
        info.isEmptyInit = parens->getNumExprs() == 0;
        info.shouldSkip =
          type->isDependentType() && parens->getNumExprs() == 1 && parens->getExpr(0)->getType()->isNullPtrType();
      }

      return info;
    }
  } // namespace

  void LocalInitializationStyleCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto const* var = result.Nodes.getNodeAs<VarDecl>("var");
    auto const* init = result.Nodes.getNodeAs<Expr>("init");

    if (!isEligibleLocalVar(var, sm, result.Context))
    {
      if (isEligibleStaticVar(var, sm))
      {
        checkStaticPrimitive(var, init);
      }

      return;
    }

    SourceLocation const loc = var->getLocation();
    QualType const varType = var->getType();
    bool const hasAuto = hasAutoTypeSpelling(var);

    if (bool const primitive = isPrimitiveType(varType); !primitive)
    {
      if (hasAuto)
      {
        return;
      }

      auto const info = analyzeInitialization(init, varType);

      if (info.shouldSkip)
      {
        return;
      }

      bool const isContainer = isContainerWithInitializerList(varType);
      bool const isString = isStringOrStringView(varType);

      if (bool const hasLiteral = hasStringLiteralArgument(init); isString && hasLiteral)
      {
        auto const suffix = StringRef{varType.getAsString().contains("string_view") ? "sv" : "s"};
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
    else if (hasAuto)
    {
      diagnosePrimitiveAutoType(var, init);
      return;
    }
    else if (var->getInitStyle() != VarDecl::CInit)
    {
      diag(loc, "primitive type should use assignment-style initialization 'Type %0 = ...'") << var->getName();
    }
  }

  void LocalInitializationStyleCheck::checkStaticPrimitive(VarDecl const* var, Expr const* init)
  {
    if (hasAutoTypeSpelling(var))
    {
      diagnosePrimitiveAutoType(var, init);
    }
    else if (init != nullptr && var->getInitStyle() != VarDecl::CInit)
    {
      diag(var->getLocation(), "primitive type should use assignment-style initialization 'Type %0 = ...'")
        << var->getName();
    }
  }

  void LocalInitializationStyleCheck::diagnosePrimitiveAutoType(VarDecl const* var, Expr const* init)
  {
    QualType const varType = var->getType();
    SourceLocation const loc = var->getLocation();

    if (!varType->isArithmeticType())
    {
      return;
    }

    if (isAllowedPrimitiveAutoInitializer(init))
    {
      return;
    }

    if (!shouldDiagnosePrimitiveAutoInitializer(init))
    {
      return;
    }

    diag(loc, "primitive type should use an explicit type instead of auto");
  }
} // namespace clang::tidy::readability
