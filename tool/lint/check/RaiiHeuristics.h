// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchersInternal.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace clang::tidy::aobus
{
  namespace detail
  {
    inline constexpr std::string_view kRaiiSuffixPattern =
      "::.*(Guard|Subscription|Scope|Session|Lock|Transaction|Timer|Writer|Reader|Changes|Tasks|Future|Handle|"
      "TempDir|TempFile|Token|Raii|Blocker)$";

    inline constexpr auto kRaiiSuffixes = std::to_array<std::string_view>({"Guard",
                                                                           "Subscription",
                                                                           "Scope",
                                                                           "Session",
                                                                           "Lock",
                                                                           "Transaction",
                                                                           "Timer",
                                                                           "Writer",
                                                                           "Reader",
                                                                           "Changes",
                                                                           "Tasks",
                                                                           "Future",
                                                                           "Handle",
                                                                           "TempDir",
                                                                           "TempFile",
                                                                           "Token",
                                                                           "Raii",
                                                                           "Blocker"});

    bool isScopedOrRaiiType(QualType type, ASTContext& context);
    bool ownsScopedOrRaiiType(QualType type, ASTContext& context);

    struct IsRAIIMatcher final : public ast_matchers::internal::MatcherInterface<CXXRecordDecl>
    {
      bool matches(CXXRecordDecl const& node,
                   ast_matchers::internal::ASTMatchFinder* /*finder*/,
                   ast_matchers::internal::BoundNodesTreeBuilder* /*builder*/) const override
      {
        bool hasUserProvidedDtor = false;
        bool hasDeletedCopyCtor = false;

        for (auto const* method : node.methods())
        {
          if (auto const* dtor = llvm::dyn_cast<CXXDestructorDecl>(method); dtor != nullptr && dtor->isUserProvided())
          {
            hasUserProvidedDtor = true;
          }
        }

        for (auto const* ctor : node.ctors())
        {
          if (ctor->isCopyConstructor() && ctor->isDeleted())
          {
            hasDeletedCopyCtor = true;
          }
        }

        auto const hasRaiiMember = std::ranges::any_of(
          node.fields(),
          [&node](FieldDecl const* field) { return ownsScopedOrRaiiType(field->getType(), node.getASTContext()); });
        auto const hasRaiiBase = std::ranges::any_of(
          node.bases(),
          [&node](CXXBaseSpecifier const& base) { return ownsScopedOrRaiiType(base.getType(), node.getASTContext()); });
        return (hasUserProvidedDtor || hasRaiiMember || hasRaiiBase) && hasDeletedCopyCtor;
      }
    };
  } // namespace detail

  inline ast_matchers::internal::Matcher<CXXRecordDecl> isRAII()
  {
    return ast_matchers::internal::Matcher{new detail::IsRAIIMatcher{}};
  }

  inline ast_matchers::internal::Matcher<CXXRecordDecl> isWhitelistedRaiiName()
  {
    return ast_matchers::anyOf(
      ast_matchers::matchesName(detail::kRaiiSuffixPattern), ast_matchers::hasName("::ao::media::file::File"));
  }

  namespace detail
  {
    inline bool isRaiiName(llvm::StringRef name)
    {
      if (name == "std::unique_ptr" || name == "std::shared_ptr" || name == "std::future" ||
          name == "ao::media::file::File")
      {
        return true;
      }

      if (name.starts_with("std::") && (name.contains("lock_guard") || name.contains("unique_lock") ||
                                        name.contains("shared_lock") || name.contains("scoped_lock")))
      {
        return true;
      }

      return std::ranges::any_of(kRaiiSuffixes, [&name](std::string_view suffix) { return name.ends_with(suffix); });
    }

    inline bool isScopedOrRaiiType(QualType type, ASTContext& context)
    {
      if (type.isNull())
      {
        return false;
      }

      // Strip raw pointers and references
      while (type->isPointerType() || type->isReferenceType())
      {
        type = type->getPointeeType();
      }

      auto const desugared = type.getDesugaredType(context);
      auto const* underlyingType = desugared.getTypePtr();

      if (auto const* specializationType = underlyingType->getAs<TemplateSpecializationType>();
          specializationType != nullptr)
      {
        for (auto const& argument : specializationType->template_arguments())
        {
          if (argument.getKind() == TemplateArgument::Type && isScopedOrRaiiType(argument.getAsType(), context))
          {
            return true;
          }
        }

        if (auto const* templateDecl = specializationType->getTemplateName().getAsTemplateDecl();
            templateDecl != nullptr && isRaiiName(templateDecl->getQualifiedNameAsString()))
        {
          return true;
        }
      }

      auto const* recordDecl = underlyingType->getAsCXXRecordDecl();

      if (recordDecl == nullptr)
      {
        return false;
      }

      // 1. Recursively check all template arguments.
      // This elegantly and automatically protects wrappers like std::optional<T>, std::unique_ptr<T>,
      // std::shared_ptr<T>.
      if (auto const* specDecl = llvm::dyn_cast<ClassTemplateSpecializationDecl>(recordDecl); specDecl != nullptr)
      {
        auto const& args = specDecl->getTemplateArgs();

        for (std::uint32_t i = 0; i < args.size(); ++i)
        {
          if (args[i].getKind() == TemplateArgument::Type)
          {
            if (isScopedOrRaiiType(args[i].getAsType(), context))
            {
              return true;
            }
          }
        }
      }

      return isRaiiName(recordDecl->getQualifiedNameAsString());
    }

    inline bool ownsScopedOrRaiiType(QualType type, ASTContext& context)
    {
      if (type.isNull() || type->isPointerType() || type->isReferenceType())
      {
        return false;
      }

      return isScopedOrRaiiType(type, context);
    }
  } // namespace detail

  inline bool isScopedOrRaiiType(QualType type, ASTContext& context)
  {
    return detail::isScopedOrRaiiType(type, context);
  }
} // namespace clang::tidy::aobus
