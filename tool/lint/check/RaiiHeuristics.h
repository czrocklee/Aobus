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
#include <string>
#include <string_view>

namespace clang::tidy::aobus
{
  namespace detail
  {
    inline constexpr std::string_view kRaiiSuffixPattern =
      "::.*(Guard|Subscription|Scope|Session|Lock|Transaction|Timer|Writer|Reader|Changes|Tasks|Handle|TempDir|"
      "TempFile|Token|Raii)$";

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
                                                                           "Handle",
                                                                           "TempDir",
                                                                           "TempFile",
                                                                           "Token",
                                                                           "Raii"});

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
          if (auto const* dtor = llvm::dyn_cast<CXXDestructorDecl>(method); dtor != nullptr)
          {
            if (dtor->isUserProvided())
            {
              hasUserProvidedDtor = true;
            }
          }
        }

        for (auto const* ctor : node.ctors())
        {
          if (ctor->isCopyConstructor() && ctor->isDeleted())
          {
            hasDeletedCopyCtor = true;
          }
        }

        return hasUserProvidedDtor && hasDeletedCopyCtor;
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
      ast_matchers::matchesName(detail::kRaiiSuffixPattern), ast_matchers::hasName("::ao::tag::TagFile"));
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

    auto const* recordDecl = underlyingType->getAsCXXRecordDecl();

    if (recordDecl == nullptr)
    {
      return false;
    }

    // 1. Recursively check all template arguments.
    // This elegantly and automatically protects wrappers like std::optional<T>, std::unique_ptr<T>, std::shared_ptr<T>.
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

    auto const name = recordDecl->getQualifiedNameAsString();

    // 2. Check if it's TagFile
    if (name == "ao::tag::TagFile")
    {
      return true;
    }

    // 3. Check for standard library locks
    if (llvm::StringRef{name}.starts_with("std::"))
    {
      if (name.find("lock_guard") != std::string::npos || name.find("unique_lock") != std::string::npos ||
          name.find("shared_lock") != std::string::npos || name.find("scoped_lock") != std::string::npos)
      {
        return true;
      }
    }

    // 4. Check naming heuristic suffixes
    return std::ranges::any_of(
      detail::kRaiiSuffixes, [&name](std::string_view suffix) { return llvm::StringRef{name}.ends_with(suffix); });
  }
} // namespace clang::tidy::aobus
