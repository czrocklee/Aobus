#include "check/BracedInitializationCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Lexer.h"

#include <llvm/ADT/StringSwitch.h>

#include <cstdint>
#include <optional>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    bool isAmbiguousContainer(QualType type)
    {
      if (type.isNull())
      {
        return false;
      }

      auto const* record = type.getCanonicalType()->getAsCXXRecordDecl();

      if (record == nullptr)
      {
        return false;
      }

      StringRef name = record->getName();

      if (name.empty())
      {
        if (auto const* spec = dyn_cast<ClassTemplateSpecializationDecl>(record))
        {
          name = spec->getSpecializedTemplate()->getName();
        }
      }

      return llvm::StringSwitch<bool>{name}
        .Cases("vector", "deque", "list", "forward_list", true)
        .Cases("basic_string", "string", true)
        .Default(false);
    }

    bool isDangerousInitialization(QualType type, CXXConstructExpr const* construct)
    {
      if (construct == nullptr)
      {
        return false;
      }

      if (!isAmbiguousContainer(type))
      {
        return false;
      }

      std::uint32_t numSourceArgs = 0;
      Expr const* firstArg = nullptr;

      for (std::uint32_t i = 0; i < construct->getNumArgs(); ++i)
      {
        if (Expr const* const arg = construct->getArg(i); arg != nullptr && !isa<CXXDefaultArgExpr>(arg))
        {
          if (firstArg == nullptr)
          {
            firstArg = arg;
          }

          numSourceArgs++;
        }
      }

      if (numSourceArgs == 0)
      {
        return false;
      }

      if (numSourceArgs >= 2)
      {
        return true;
      }

      if (numSourceArgs == 1 && firstArg != nullptr)
      {
        QualType const argType = firstArg->IgnoreImplicit()->getType().getCanonicalType();

        if (argType->isIntegerType())
        {
          return true;
        }
      }

      return false;
    }

    bool isScalarType(QualType type)
    {
      if (type.isNull())
      {
        return false;
      }

      QualType const canon = type.getCanonicalType();
      return canon->isArithmeticType() || canon->isAnyCharacterType() || canon->isPointerType() ||
             canon->isEnumeralType();
    }

    bool isHandledByLocalStyleCheck(VarDecl const* var, CXXConstructExpr const* construct)
    {
      if (var == nullptr || construct == nullptr)
      {
        return false;
      }

      if (isScalarType(var->getType().getCanonicalType()))
      {
        return true;
      }

      if (construct->getNumArgs() >= 1)
      {
        if (auto const* arg0 = construct->getArg(0); arg0 != nullptr)
        {
          if (Expr const* const ignoredArg0 = arg0->IgnoreImplicit();
              ignoredArg0 != nullptr && (isa<StringLiteral>(ignoredArg0) || isa<CXXBindTemporaryExpr>(ignoredArg0)))
          {
            if (var->getType().getCanonicalType().getAsString().find("string") != std::string::npos)
            {
              return true;
            }
          }
        }
      }

      return false;
    }

    bool isParenChar(SourceLocation loc, SourceManager const& sm, char expected)
    {
      if (loc.isInvalid())
      {
        return false;
      }

      bool invalid = false;
      SourceLocation const spellLoc = sm.getSpellingLoc(loc);
      char const* data = sm.getCharacterData(spellLoc, &invalid);

      if (invalid || data == nullptr)
      {
        return false;
      }

      if (*data == expected)
      {
        return true;
      }

      static constexpr std::int32_t kScanNeighborhood = 3;

      for (std::int32_t i = 1; i <= kScanNeighborhood; ++i)
      {
        if (*(data + i) == expected)
        {
          return true;
        }

        if (*(data - i) == expected)
        {
          return true;
        }
      }

      return false;
    }

    struct BraceTarget final
    {
      SourceLocation lParen;
      SourceLocation rParen;
      std::string name;
    };

    std::optional<BraceTarget> analyzeCtorInit(CXXCtorInitializer const* init)
    {
      auto optTarget = BraceTarget{.lParen = init->getLParenLoc(), .rParen = init->getRParenLoc(), .name = ""};

      if (auto const* member = init->getMember())
      {
        optTarget.name = member->getNameAsString();
      }
      else if (auto const* base = init->getBaseClass())
      {
        if (auto const* record = base->getAsCXXRecordDecl())
        {
          optTarget.name = record->getNameAsString();
        }
        else if (auto const* type = base->getAs<TypedefType>())
        {
          optTarget.name = type->getDecl()->getNameAsString();
        }
      }

      if (auto const* construct = dyn_cast_or_null<CXXConstructExpr>(init->getInit()))
      {
        QualType const initType =
          init->getTypeSourceInfo() != nullptr ? init->getTypeSourceInfo()->getType() : construct->getType();

        if (isDangerousInitialization(initType, construct))
        {
          return std::nullopt;
        }
      }

      return optTarget;
    }

    std::optional<BraceTarget> analyzeFunctionalCast(CXXFunctionalCastExpr const* cast)
    {
      if (isScalarType(cast->getType()))
      {
        return std::nullopt;
      }

      auto const* construct = dyn_cast_or_null<CXXConstructExpr>(cast->getSubExpr()->IgnoreImplicit());

      if (construct == nullptr)
      {
        if (auto const* bind = dyn_cast_or_null<CXXBindTemporaryExpr>(cast->getSubExpr()->IgnoreImplicit()))
        {
          construct = dyn_cast_or_null<CXXConstructExpr>(bind->getSubExpr()->IgnoreImplicit());
        }
      }

      if (isDangerousInitialization(cast->getType(), construct))
      {
        return std::nullopt;
      }

      return BraceTarget{
        .lParen = cast->getLParenLoc(), .rParen = cast->getRParenLoc(), .name = cast->getType().getAsString()};
    }

    std::optional<BraceTarget> analyzeTempObject(CXXTemporaryObjectExpr const* temp)
    {
      if (isScalarType(temp->getType()) || isDangerousInitialization(temp->getType(), temp))
      {
        return std::nullopt;
      }

      auto const range = temp->getParenOrBraceRange();
      auto optTarget = BraceTarget{.lParen = range.getBegin(), .rParen = range.getEnd(), .name = ""};

      if (auto const* ctor = temp->getConstructor())
      {
        optTarget.name = ctor->getParent()->getNameAsString();
      }

      return optTarget;
    }

    std::optional<BraceTarget> analyzeNewExpr(MatchFinder::MatchResult const& result)
    {
      auto const* construct = result.Nodes.getNodeAs<CXXConstructExpr>("new_construct");

      if (construct == nullptr || isDangerousInitialization(construct->getType(), construct))
      {
        return std::nullopt;
      }

      auto const range = construct->getParenOrBraceRange();
      return BraceTarget{
        .lParen = range.getBegin(), .rParen = range.getEnd(), .name = construct->getType().getAsString()};
    }

    std::optional<BraceTarget> analyzeVarDecl(VarDecl const* var, MatchFinder::MatchResult const& result)
    {
      auto const* construct = result.Nodes.getNodeAs<CXXConstructExpr>("var_construct");

      if (construct == nullptr || isDangerousInitialization(var->getType(), construct) ||
          isHandledByLocalStyleCheck(var, construct))
      {
        return std::nullopt;
      }

      if (var->getInitStyle() != VarDecl::CallInit)
      {
        return std::nullopt;
      }

      auto const range = construct->getParenOrBraceRange();
      return BraceTarget{.lParen = range.getBegin(), .rParen = range.getEnd(), .name = var->getNameAsString()};
    }
  } // namespace

  void BracedInitializationCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(cxxCtorInitializer(isWritten()).bind("ctor_init"), this);
    finder->addMatcher(cxxTemporaryObjectExpr(unless(isListInitialization())).bind("temp_obj"), this);
    finder->addMatcher(cxxFunctionalCastExpr(unless(hasDescendant(initListExpr()))).bind("func_cast"), this);
    finder->addMatcher(
      cxxNewExpr(has(cxxConstructExpr(unless(isListInitialization())).bind("new_construct"))).bind("new_expr"), this);
    finder->addMatcher(varDecl(unless(isImplicit()),
                               unless(parmVarDecl()),
                               hasInitializer(cxxConstructExpr(unless(isListInitialization())).bind("var_construct")))
                         .bind("var_decl"),
                       this);
  }

  void BracedInitializationCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const& sm = *result.SourceManager;
    auto optTarget = std::optional<BraceTarget>{};

    if (auto const* init = result.Nodes.getNodeAs<CXXCtorInitializer>("ctor_init"))
    {
      optTarget = analyzeCtorInit(init);
    }
    else if (auto const* cast = result.Nodes.getNodeAs<CXXFunctionalCastExpr>("func_cast"))
    {
      optTarget = analyzeFunctionalCast(cast);
    }
    else if (auto const* temp = result.Nodes.getNodeAs<CXXTemporaryObjectExpr>("temp_obj"))
    {
      optTarget = analyzeTempObject(temp);
    }
    else if (result.Nodes.getNodeAs<CXXNewExpr>("new_expr") != nullptr)
    {
      optTarget = analyzeNewExpr(result);
    }
    else if (auto const* var = result.Nodes.getNodeAs<VarDecl>("var_decl"))
    {
      optTarget = analyzeVarDecl(var, result);
    }

    if (!optTarget || optTarget->lParen.isInvalid() || optTarget->rParen.isInvalid())
    {
      return;
    }

    if (!isParenChar(optTarget->lParen, sm, '(') || isParenChar(optTarget->lParen, sm, '{'))
    {
      return;
    }

    auto diagBuilder = diag(optTarget->lParen, "use brace initialization '%0{...}' instead of parentheses")
                       << optTarget->name;
    diagBuilder << FixItHint::CreateReplacement(
                     CharSourceRange::getCharRange(optTarget->lParen, optTarget->lParen.getLocWithOffset(1)), "{")
                << FixItHint::CreateReplacement(
                     CharSourceRange::getCharRange(optTarget->rParen, optTarget->rParen.getLocWithOffset(1)), "}");
  }
} // namespace clang::tidy::readability
