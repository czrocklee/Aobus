// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "check/ExplicitPathConversionCheck.h"

#include "check/AstHelpers.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>

#include <algorithm>
#include <string>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    constexpr unsigned char kNonAsciiByte = 0x80U;

    bool isFilesystemPath(CXXRecordDecl const& record)
    {
      if (record.getName() != "path")
      {
        return false;
      }

      auto const name = record.getQualifiedNameAsString();
      return name == "std::filesystem::path" || (name.starts_with("std::filesystem::") && name.ends_with("::path"));
    }

    bool returnsNarrowString(CXXMethodDecl const& method, ASTContext const& context)
    {
      auto const* record = method.getReturnType().getCanonicalType()->getAsCXXRecordDecl();
      auto const* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);

      if (specialization == nullptr)
      {
        return false;
      }

      auto const& arguments = specialization->getTemplateArgs();
      return arguments.size() > 0 && arguments[0].getKind() == TemplateArgument::Type &&
             ASTContext::hasSameType(arguments[0].getAsType(), context.CharTy);
    }

    bool isNarrowTextType(QualType candidateType, ASTContext const& context)
    {
      candidateType = candidateType.getNonReferenceType().getCanonicalType().getUnqualifiedType();

      if (auto const* pointer = candidateType->getAs<PointerType>(); pointer != nullptr)
      {
        return ASTContext::hasSameType(pointer->getPointeeType().getUnqualifiedType(), context.CharTy);
      }

      if (auto const* array = context.getAsArrayType(candidateType); array != nullptr)
      {
        return ASTContext::hasSameType(array->getElementType().getUnqualifiedType(), context.CharTy);
      }

      auto const* record = candidateType->getAsCXXRecordDecl();
      auto const* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);

      if (specialization == nullptr)
      {
        return false;
      }

      auto const& arguments = specialization->getTemplateArgs();
      return arguments.size() > 0 && arguments[0].getKind() == TemplateArgument::Type &&
             ASTContext::hasSameType(arguments[0].getAsType(), context.CharTy);
    }

    bool isAsciiLiteral(Expr const& expression)
    {
      auto const* source = aobus::stripImplicitNodes(&expression);
      auto const* literal =
        dyn_cast_or_null<StringLiteral>(source != nullptr ? source->IgnoreParenImpCasts() : nullptr);

      if (literal == nullptr)
      {
        return false;
      }

      return std::ranges::all_of(
        literal->getBytes(), [](char const byte) { return static_cast<unsigned char>(byte) < kNonAsciiByte; });
    }
  } // namespace

  void ExplicitPathConversionCheck::registerMatchers(MatchFinder* finder)
  {
    finder->addMatcher(cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("string", "generic_string"),
                                                              ofClass(cxxRecordDecl(hasName("path"))))))
                         .bind("pathStringCall"),
                       this);

    finder->addMatcher(cxxConstructExpr(hasDeclaration(cxxConstructorDecl(ofClass(cxxRecordDecl(hasName("path"))))))
                         .bind("pathConstruction"),
                       this);
  }

  void ExplicitPathConversionCheck::check(MatchFinder::MatchResult const& result)
  {
    if (auto const* construction = result.Nodes.getNodeAs<CXXConstructExpr>("pathConstruction");
        construction != nullptr)
    {
      auto const* constructor = construction->getConstructor();
      auto const* record = constructor != nullptr ? constructor->getParent() : nullptr;

      if (record != nullptr && result.Context != nullptr && construction->getNumArgs() >= 1 &&
          isFilesystemPath(*record) && isNarrowTextType(construction->getArg(0)->getType(), *result.Context) &&
          !isAsciiLiteral(*construction->getArg(0)) &&
          aobus::isPolicySource(*result.SourceManager, construction->getBeginLoc()))
      {
        diag(construction->getBeginLoc(),
             "std::filesystem::path construction from narrow text has an ambient encoding; use pathFromUtf8() "
             "for UTF-8 text or pathFromNative() for native filename bytes");
      }

      return;
    }

    auto const* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("pathStringCall");
    auto const* method = call != nullptr ? call->getMethodDecl() : nullptr;
    auto const* record = method != nullptr ? method->getParent() : nullptr;

    if (call == nullptr || method == nullptr || record == nullptr || result.Context == nullptr ||
        !isFilesystemPath(*record) || !returnsNarrowString(*method, *result.Context) ||
        !aobus::isPolicySource(*result.SourceManager, call->getBeginLoc()))
    {
      return;
    }

    diag(call->getBeginLoc(),
         "std::filesystem::path narrow string conversion has an ambient encoding; use pathToUtf8() or "
         "pathToGenericUtf8() for text, and native() for a native filesystem API");
  }
} // namespace clang::tidy::readability
