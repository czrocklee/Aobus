// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/UseStdNumbersCheck.h"

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyOptions.h>
#include <clang-tidy/utils/IncludeSorter.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

#include <optional>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  UseStdNumbersCheck::UseStdNumbersCheck(StringRef name, ClangTidyContext* context)
    : ClangTidyCheck{name, context}, _includeInserter{utils::IncludeSorter::IS_LLVM, true}
  {
  }

  void UseStdNumbersCheck::storeOptions(ClangTidyOptions::OptionMap& opts)
  {
    Options.store(opts, "IncludeStyle", "llvm");
  }

  void UseStdNumbersCheck::registerPPCallbacks(SourceManager const& /*sm*/,
                                               Preprocessor* pp,
                                               Preprocessor* /*moduleExpanderPP*/)
  {
    _includeInserter.registerPreprocessor(pp);
  }

  void UseStdNumbersCheck::registerMatchers(MatchFinder* finder)
  {
    auto builtinLoc = typeLoc(loc(builtinType())).bind("tl");
    auto builtinTypeMatcher = builtinType().bind("bt");

    finder->addMatcher(varDecl(hasTypeLoc(builtinLoc)).bind("var"), this);
    finder->addMatcher(fieldDecl(hasTypeLoc(builtinLoc)).bind("field"), this);
    finder->addMatcher(parmVarDecl(hasTypeLoc(builtinLoc)).bind("parm"), this);
    finder->addMatcher(functionDecl(hasReturnTypeLoc(builtinLoc)).bind("func_ret"), this);
    finder->addMatcher(explicitCastExpr(hasDestinationType(builtinTypeMatcher)).bind("cast"), this);
  }

  namespace
  {
    bool isOverriddenMethod(Decl const* decl)
    {
      if (auto const* func = llvm::dyn_cast_or_null<CXXMethodDecl>(decl); func != nullptr)
      {
        return func->size_overridden_methods() > 0;
      }

      if (auto const* parm = llvm::dyn_cast_or_null<ParmVarDecl>(decl); parm != nullptr)
      {
        if (auto const* func = llvm::dyn_cast_or_null<CXXMethodDecl>(parm->getDeclContext()); func != nullptr)
        {
          return func->size_overridden_methods() > 0;
        }
      }

      return false;
    }

    bool hasCStyleLinkage(Decl const* decl)
    {
      if (auto const* func = llvm::dyn_cast_or_null<FunctionDecl>(decl); func != nullptr)
      {
        return func->isExternC();
      }

      if (auto const* parm = llvm::dyn_cast_or_null<ParmVarDecl>(decl); parm != nullptr)
      {
        if (auto const* func = llvm::dyn_cast_or_null<FunctionDecl>(parm->getDeclContext()); func != nullptr)
        {
          return func->isExternC();
        }
      }

      if (auto const* var = llvm::dyn_cast_or_null<VarDecl>(decl); var != nullptr)
      {
        return var->isExternC();
      }

      return false;
    }

    bool isMainFunction(Decl const* decl)
    {
      if (auto const* func = llvm::dyn_cast_or_null<FunctionDecl>(decl); func != nullptr)
      {
        return func->isMain();
      }

      if (auto const* parm = llvm::dyn_cast_or_null<ParmVarDecl>(decl); parm != nullptr)
      {
        if (auto const* func = llvm::dyn_cast_or_null<FunctionDecl>(parm->getDeclContext()); func != nullptr)
        {
          return func->isMain();
        }
      }

      return false;
    }

    struct ExternCUsageVisitor : public RecursiveASTVisitor<ExternCUsageVisitor>
    {
      ValueDecl const* targetDecl;
      bool found = false;

      explicit ExternCUsageVisitor(ValueDecl const* target)
        : targetDecl{target}
      {
      }

      bool VisitCallExpr(CallExpr* call) // NOLINT(readability-identifier-naming)
      {
        if (auto const* calleeDecl = call->getDirectCallee(); calleeDecl != nullptr)
        {
          if (calleeDecl->isExternC())
          {
            for (auto const* arg : call->arguments())
            {
              if (mentionsDecl(arg))
              {
                found = true;
                return false; // Stop traversing
              }
            }
          }
        }

        return true;
      }

      bool mentionsDecl(Expr const* expr) const
      {
        if (expr == nullptr)
        {
          return false;
        }

        expr = expr->IgnoreParenImpCasts();

        if (auto const* declRef = llvm::dyn_cast<DeclRefExpr>(expr); declRef != nullptr)
        {
          return declRef->getDecl() == targetDecl;
        }

        if (auto const* memberExpr = llvm::dyn_cast<MemberExpr>(expr); memberExpr != nullptr)
        {
          return memberExpr->getMemberDecl() == targetDecl;
        }

        if (auto const* unOp = llvm::dyn_cast<UnaryOperator>(expr); unOp != nullptr)
        {
          return mentionsDecl(unOp->getSubExpr());
        }

        return false;
      }
    };

    bool isPassedToExternC(ValueDecl const* decl)
    {
      if (auto const* var = llvm::dyn_cast<VarDecl>(decl); var != nullptr)
      {
        if (auto const* declCtx = var->getDeclContext(); declCtx != nullptr && declCtx->isFunctionOrMethod())
        {
          if (auto const* func = llvm::dyn_cast<FunctionDecl>(declCtx); func != nullptr)
          {
            if (func->hasBody())
            {
              auto visitor = ExternCUsageVisitor{var};
              visitor.TraverseStmt(func->getBody());
              return visitor.found;
            }
          }
        }
      }
      else if (auto const* field = llvm::dyn_cast<FieldDecl>(decl); field != nullptr)
      {
        auto const* record = field->getParent();

        for (auto const* member : record->decls())
        {
          if (auto const* method = llvm::dyn_cast<FunctionDecl>(member); method != nullptr)
          {
            if (method->hasBody())
            {
              auto visitor = ExternCUsageVisitor{field};
              visitor.TraverseStmt(method->getBody());

              if (visitor.found)
              {
                return true;
              }
            }
          }
        }
      }

      return false;
    }

    struct MatchContext final
    {
      BuiltinTypeLoc builtinLoc;
      SourceLocation loc;
      Type const* type;
      Decl const* contextDecl;
      bool isCast;
    };

    std::optional<MatchContext> getMatchContext(MatchFinder::MatchResult const& result)
    {
      if (auto const* tl = result.Nodes.getNodeAs<TypeLoc>("tl"); tl != nullptr)
      {
        auto const builtinLoc = tl->getAs<BuiltinTypeLoc>();

        if (builtinLoc.isNull())
        {
          return std::nullopt;
        }

        auto const* contextDecl = static_cast<Decl const*>(nullptr);

        if (auto const* decl = result.Nodes.getNodeAs<VarDecl>("var"); decl != nullptr)
        {
          contextDecl = decl;
        }
        else if (auto const* decl = result.Nodes.getNodeAs<FieldDecl>("field"); decl != nullptr)
        {
          contextDecl = decl;
        }
        else if (auto const* decl = result.Nodes.getNodeAs<ParmVarDecl>("parm"); decl != nullptr)
        {
          contextDecl = decl;
        }
        else if (auto const* decl = result.Nodes.getNodeAs<FunctionDecl>("func_ret"); decl != nullptr)
        {
          contextDecl = decl;
        }

        return MatchContext{.builtinLoc = builtinLoc,
                            .loc = builtinLoc.getBeginLoc(),
                            .type = builtinLoc.getTypePtr(),
                            .contextDecl = contextDecl,
                            .isCast = false};
      }

      if (auto const* expr = result.Nodes.getNodeAs<ExplicitCastExpr>("cast"); expr != nullptr)
      {
        if (auto* const typeInfo = expr->getTypeInfoAsWritten(); typeInfo != nullptr)
        {
          auto const builtinLoc = typeInfo->getTypeLoc().getAs<BuiltinTypeLoc>();

          if (!builtinLoc.isNull())
          {
            return MatchContext{.builtinLoc = builtinLoc,
                                .loc = builtinLoc.getBeginLoc(),
                                .type = builtinLoc.getTypePtr(),
                                .contextDecl = nullptr,
                                .isCast = true};
          }
        }
      }

      return std::nullopt;
    }

    bool shouldSkip(MatchContext const& match, SourceManager const& sm)
    {
      if (match.loc.isInvalid() || match.loc.isMacroID() || sm.isInSystemHeader(match.loc))
      {
        return true;
      }

      if (match.contextDecl != nullptr)
      {
        if (isMainFunction(match.contextDecl) || hasCStyleLinkage(match.contextDecl) ||
            isOverriddenMethod(match.contextDecl))
        {
          return true;
        }

        if (auto const* valDecl = llvm::dyn_cast<ValueDecl>(match.contextDecl); valDecl != nullptr)
        {
          if (isPassedToExternC(valDecl))
          {
            return true;
          }
        }
      }

      return false;
    }
  } // namespace

  void UseStdNumbersCheck::check(MatchFinder::MatchResult const& result)
  {
    auto const optMatch = getMatchContext(result);

    if (!optMatch)
    {
      return;
    }

    auto const& sm = *result.SourceManager;

    if (shouldSkip(*optMatch, sm))
    {
      return;
    }

    auto const kind = optMatch->type->getAs<BuiltinType>()->getKind();
    auto replacement = StringRef{};
    bool isLong = false;

    switch (kind)
    {
      case BuiltinType::Short: replacement = "std::int16_t"; break;
      case BuiltinType::UShort: replacement = "std::uint16_t"; break;
      case BuiltinType::Int: replacement = "std::int32_t"; break;
      case BuiltinType::UInt: replacement = "std::uint32_t"; break;
      case BuiltinType::Long:
        isLong = true;
        replacement = optMatch->isCast ? "std::ptrdiff_t" : "std::int64_t";
        break;
      case BuiltinType::ULong:
        isLong = true;
        replacement = optMatch->isCast ? "std::size_t" : "std::uint64_t";
        break;
      case BuiltinType::LongLong: replacement = "std::int64_t"; break;
      case BuiltinType::ULongLong: replacement = "std::uint64_t"; break;
      default: return;
    }

    if (isLong && !optMatch->isCast)
    {
      diag(optMatch->loc,
           "prefer explicit sized integer types over '%0' (avoid auto-fix to prevent breaking external C APIs)")
        << optMatch->type->getAs<BuiltinType>()->getName(result.Context->getPrintingPolicy());
      return;
    }

    auto diagBuilder = diag(optMatch->loc, "prefer explicit sized integer type '%0' over '%1'")
                       << replacement
                       << optMatch->type->getAs<BuiltinType>()->getName(result.Context->getPrintingPolicy());

    auto const charRange = CharSourceRange::getTokenRange(optMatch->builtinLoc.getSourceRange());
    diagBuilder << FixItHint::CreateReplacement(charRange, replacement);

    if (replacement.starts_with("std::int") || replacement.starts_with("std::uint"))
    {
      if (auto const fileId = sm.getFileID(optMatch->loc); _insertedHeaders[fileId].insert("<cstdint>").second)
      {
        if (auto optFixit = _includeInserter.createIncludeInsertion(fileId, "<cstdint>"); optFixit)
        {
          diagBuilder << *optFixit;
        }
      }
    }
    else if (replacement.starts_with("std::size_t") || replacement.starts_with("std::ptrdiff_t"))
    {
      if (auto const fileId = sm.getFileID(optMatch->loc); _insertedHeaders[fileId].insert("<cstddef>").second)
      {
        if (auto optFixit = _includeInserter.createIncludeInsertion(fileId, "<cstddef>"); optFixit)
        {
          diagBuilder << *optFixit;
        }
      }
    }
  }
} // namespace clang::tidy::readability
