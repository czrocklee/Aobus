// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/LocalInitializationStyleCheck.h"

#include "check/AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Type.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringSwitch.h>

#include <string>

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
        .Cases({"vector", "deque", "list", "forward_list"}, true)
        .Cases({"set", "multiset", "unordered_set", "unordered_multiset"}, true)
        .Cases({"map", "multimap", "unordered_map", "unordered_multimap"}, true)
        .Cases({"basic_string", "string"}, true)
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

    AutoTypeLoc getAutoTypeLoc(VarDecl const* var)
    {
      if (var == nullptr)
      {
        return {};
      }

      auto const* typeInfo = var->getTypeSourceInfo();

      if (typeInfo == nullptr)
      {
        return {};
      }

      return typeInfo->getTypeLoc().getContainedAutoTypeLoc();
    }

    bool hasAutoTypeSpelling(VarDecl const* var)
    {
      return !getAutoTypeLoc(var).isNull();
    }

    bool isFixableAutoPrimitiveType(QualType type)
    {
      type = type.getNonReferenceType().getUnqualifiedType();

      if (type->isPointerType() || type->isReferenceType() || type->isNullPtrType())
      {
        return false;
      }

      return type->isArithmeticType() || type->isAnyCharacterType();
    }

    std::string getExplicitPrimitiveTypeName(QualType type, ASTContext const& context)
    {
      auto policy = PrintingPolicy{context.getPrintingPolicy()};
      policy.SuppressTagKeyword = true;

      return type.getNonReferenceType().getUnqualifiedType().getAsString(policy);
    }

    std::string getWrittenTypeName(TypeSourceInfo const& typeInfo,
                                   SourceManager const& sm,
                                   LangOptions const& langOpts,
                                   ASTContext const& context)
    {
      auto const typeRange = CharSourceRange::getTokenRange(typeInfo.getTypeLoc().getSourceRange());
      auto spelling = Lexer::getSourceText(typeRange, sm, langOpts).str();

      if (!spelling.empty())
      {
        return spelling;
      }

      return getExplicitPrimitiveTypeName(typeInfo.getType(), context);
    }

    CXXFunctionalCastExpr const* getPrimitiveFunctionalCast(Expr const* init)
    {
      auto const* functionalCast =
        dyn_cast_or_null<CXXFunctionalCastExpr>(init == nullptr ? nullptr : init->IgnoreImplicit());

      if (functionalCast == nullptr)
      {
        return nullptr;
      }

      auto const* typeInfo = functionalCast->getTypeInfoAsWritten();

      if (typeInfo == nullptr || !isFixableAutoPrimitiveType(typeInfo->getType()))
      {
        return nullptr;
      }

      return functionalCast;
    }

    Expr const* getSingleFunctionalCastArgument(CXXFunctionalCastExpr const* functionalCast)
    {
      if (functionalCast == nullptr)
      {
        return nullptr;
      }

      auto const* argument = functionalCast->getSubExprAsWritten();

      if (auto const* initList = dyn_cast_or_null<InitListExpr>(argument); initList != nullptr)
      {
        if (initList->getNumInits() != 1)
        {
          return nullptr;
        }

        return initList->getInit(0);
      }

      return argument;
    }

    bool isAllowedPrimitiveAutoInitializer(Expr const* init)
    {
      return isa_and_nonnull<CXXStaticCastExpr>(init == nullptr ? nullptr : init->IgnoreImplicit());
    }

    bool isPrimitiveLiteralInitializer(Expr const* init)
    {
      auto const* clean = aobus::stripImplicitNodes(init);

      if (clean == nullptr)
      {
        return false;
      }

      clean = clean->IgnoreParens();

      if (auto const* unary = dyn_cast<UnaryOperator>(clean); unary != nullptr)
      {
        if (unary->getOpcode() == UO_Plus || unary->getOpcode() == UO_Minus)
        {
          clean = unary->getSubExpr()->IgnoreParens();
        }
      }

      return isa<IntegerLiteral, FloatingLiteral, CXXBoolLiteralExpr, CharacterLiteral>(clean);
    }

    bool shouldDiagnosePrimitiveAutoInitializer(Expr const* init)
    {
      return getPrimitiveFunctionalCast(init) != nullptr || isPrimitiveLiteralInitializer(init);
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

    // Primitive initialization (Rule 3.4.5: primitives use assignment-style).
    // unless(isImplicit()) skips compiler-synthesized VarDecls such as the
    // coroutine parameter-move copies, whose location lands on the first
    // coroutine keyword of the body and which have no stylable spelling.
    finder->addMatcher(
      varDecl(unless(parmVarDecl()),
              unless(isImplicit()),
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
      else if (isa<InitListExpr>(init->IgnoreImplicit()))
      {
        info.isListInit = true;
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

      auto const info = analyzeInitialization(init);

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
      diagnosePrimitiveAutoType(var, init, sm, *result.Context);
      return;
    }
    else if (var->getInitStyle() != VarDecl::CInit)
    {
      diag(loc, "primitive type should use assignment-style initialization 'Type %0 = ...'") << var->getName();
    }
  }
  void LocalInitializationStyleCheck::diagnosePrimitiveAutoType(VarDecl const* var,
                                                                Expr const* init,
                                                                SourceManager const& sm,
                                                                ASTContext& context)
  {
    QualType const varType = var->getType();
    SourceLocation const loc = var->getLocation();

    if (!isFixableAutoPrimitiveType(varType))
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

    auto const autoLoc = getAutoTypeLoc(var);
    auto const* functionalCast = getPrimitiveFunctionalCast(init);
    auto const replacement =
      functionalCast != nullptr
        ? getWrittenTypeName(*functionalCast->getTypeInfoAsWritten(), sm, context.getLangOpts(), context)
        : getExplicitPrimitiveTypeName(varType, context);
    auto diagnostic = diag(loc, "primitive type should use explicit type '%0' instead of auto") << replacement;

    if (!autoLoc.isDecltypeAuto() && !autoLoc.isConstrained() && !aobus::isInMacro(autoLoc.getSourceRange()))
    {
      diagnostic << FixItHint::CreateReplacement(CharSourceRange::getTokenRange(autoLoc.getSourceRange()), replacement);

      auto const* singleArgument = getSingleFunctionalCastArgument(functionalCast);

      if (singleArgument != nullptr && !aobus::isInMacro(functionalCast->getSourceRange()) &&
          !aobus::isInMacro(singleArgument->getSourceRange()))
      {
        diagnostic << FixItHint::CreateReplacement(
          CharSourceRange::getTokenRange(functionalCast->getSourceRange()),
          aobus::getExprSourceText(*singleArgument, sm, context.getLangOpts()));
      }
    }
  }
} // namespace clang::tidy::readability
