// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/ChronoNamingConventionCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <array>
#include <cctype>

using namespace clang::ast_matchers;

namespace clang::tidy::readability
{
  namespace
  {
    // Returns true when `name` ends with one of `approvedNouns` (case-insensitive)
    // at a word boundary: the noun spans the whole name, follows an underscore, or
    // begins a camelCase segment (its first matched character is uppercase).
    bool endsWithApprovedNoun(StringRef name, llvm::ArrayRef<StringRef> approvedNouns)
    {
      return std::ranges::any_of(
        approvedNouns,
        [&name](StringRef noun)
        {
          if (!name.ends_with_insensitive(noun))
          {
            return false;
          }

          if (name.size() == noun.size())
          {
            return true;
          }

          if (char const before = name[name.size() - noun.size() - 1];
              before == '_' || std::isupper(static_cast<unsigned char>(name[name.size() - noun.size()])) != 0)
          {
            return true;
          }

          return false;
        });
    }

    // Conversion / factory functions (fromX, toX, asX, makeX, parseX, convertX)
    // are named after their input or intent rather than their return type, so
    // they are exempt from the return-type noun convention.
    bool isConversionFunctionName(StringRef name)
    {
      static constexpr std::array kConversionPrefixes{StringRef{"from"},
                                                      StringRef{"to"},
                                                      StringRef{"as"},
                                                      StringRef{"make"},
                                                      StringRef{"parse"},
                                                      StringRef{"convert"}};

      return std::ranges::any_of(kConversionPrefixes,
                                 [&name](StringRef prefix)
                                 {
                                   if (!name.starts_with(prefix))
                                   {
                                     return false;
                                   }

                                   return name.size() == prefix.size() ||
                                          std::isupper(static_cast<unsigned char>(name[prefix.size()])) != 0;
                                 });
    }

    // Detects names that stack two time nouns, e.g. "elapsedDuration" or
    // "nowTime": the stem ("elapsed", "now") is already a complete time concept,
    // so the trailing generic suffix is redundant. Returns the stem to suggest as
    // the rename, or an empty StringRef when the name is not redundant.
    //
    // Positional words (start, end, mark, point, origin) and descriptive
    // qualifiers (frame, buffered, preroll, ...) are deliberately not "strong
    // stems", so idiomatic names like startTime or bufferedDuration are left alone.
    StringRef redundantStem(StringRef name)
    {
      static constexpr std::array kGenericSuffixes{
        StringRef{"time"}, StringRef{"duration"}, StringRef{"timestamp"}, StringRef{"instant"}};
      static constexpr std::array kStrongStems{StringRef{"elapsed"},
                                               StringRef{"now"},
                                               StringRef{"deadline"},
                                               StringRef{"timeout"},
                                               StringRef{"interval"},
                                               StringRef{"delay"},
                                               StringRef{"period"},
                                               StringRef{"remaining"},
                                               StringRef{"latency"},
                                               StringRef{"epoch"},
                                               StringRef{"expiry"},
                                               StringRef{"age"},
                                               StringRef{"span"},
                                               StringRef{"budget"},
                                               StringRef{"delta"},
                                               StringRef{"threshold"},
                                               StringRef{"offset"},
                                               StringRef{"length"}};

      for (StringRef const suffix : kGenericSuffixes)
      {
        if (!name.ends_with_insensitive(suffix) || name.size() == suffix.size())
        {
          continue;
        }

        // The suffix must begin a camelCase segment or follow an underscore.
        char const boundary = name[name.size() - suffix.size()];
        char const before = name[name.size() - suffix.size() - 1];

        if (before != '_' && std::isupper(static_cast<unsigned char>(boundary)) == 0)
        {
          continue;
        }

        StringRef stem = name.take_front(name.size() - suffix.size());

        if (before == '_')
        {
          stem = stem.drop_back();
        }

        if (endsWithApprovedNoun(stem, kStrongStems))
        {
          return stem;
        }
      }

      return {};
    }

    bool isApprovedDurationName(StringRef name)
    {
      static constexpr std::array kApprovedNouns{StringRef{"duration"},
                                                 StringRef{"interval"},
                                                 StringRef{"timeout"},
                                                 StringRef{"delay"},
                                                 StringRef{"period"},
                                                 StringRef{"time"},
                                                 StringRef{"offset"},
                                                 StringRef{"threshold"},
                                                 StringRef{"elapsed"},
                                                 StringRef{"length"},
                                                 StringRef{"latency"},
                                                 StringRef{"remaining"},
                                                 StringRef{"budget"},
                                                 StringRef{"span"},
                                                 StringRef{"age"},
                                                 StringRef{"delta"}};

      return endsWithApprovedNoun(name, kApprovedNouns);
    }

    // Short conventional instant names: `tp` (the standard "time point"
    // abbreviation) and ordered samples `t0`, `t1`, ... `tN` captured when
    // measuring an elapsed span between successive instants.
    bool isShortTimePointName(StringRef name)
    {
      if (name == "tp")
      {
        return true;
      }

      return name.size() >= 2 && name.front() == 't' &&
             std::ranges::all_of(
               name.drop_front(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
    }

    bool isApprovedTimePointName(StringRef name)
    {
      if (isShortTimePointName(name))
      {
        return true;
      }

      static constexpr std::array kApprovedNouns{StringRef{"time"},
                                                 StringRef{"timestamp"},
                                                 StringRef{"instant"},
                                                 StringRef{"deadline"},
                                                 StringRef{"point"},
                                                 StringRef{"epoch"},
                                                 StringRef{"mark"},
                                                 StringRef{"start"},
                                                 StringRef{"end"},
                                                 StringRef{"expiry"},
                                                 StringRef{"origin"},
                                                 StringRef{"now"}};

      return endsWithApprovedNoun(name, kApprovedNouns);
    }

    StringRef durationNounsList()
    {
      return "duration, interval, timeout, delay, period, time, offset, threshold, elapsed, length, latency, "
             "remaining, budget, span, age, delta";
    }

    StringRef timePointNounsList()
    {
      return "time, timestamp, instant, deadline, point, epoch, mark, start, end, expiry, origin, now";
    }
  } // namespace

  void ChronoNamingConventionCheck::registerMatchers(MatchFinder* finder)
  {
    auto chronoQualType = [](StringRef qualifiedName)
    {
      return qualType(hasCanonicalType(anyOf(
        recordType(hasDeclaration(classTemplateSpecializationDecl(hasName(qualifiedName)))),
        referenceType(pointee(
          hasCanonicalType(recordType(hasDeclaration(classTemplateSpecializationDecl(hasName(qualifiedName))))))),
        pointerType(pointee(
          hasCanonicalType(recordType(hasDeclaration(classTemplateSpecializationDecl(hasName(qualifiedName))))))))));
    };

    finder->addMatcher(declaratorDecl(hasType(chronoQualType("::std::chrono::duration"))).bind("durationDecl"), this);
    finder->addMatcher(functionDecl(returns(chronoQualType("::std::chrono::duration"))).bind("durationFunc"), this);
    finder->addMatcher(
      declaratorDecl(hasType(chronoQualType("::std::chrono::time_point"))).bind("timePointDecl"), this);
    finder->addMatcher(functionDecl(returns(chronoQualType("::std::chrono::time_point"))).bind("timePointFunc"), this);
  }

  void ChronoNamingConventionCheck::check(MatchFinder::MatchResult const& result)
  {
    auto processDecl = [this](NamedDecl const* decl, bool isFunction, bool isTimePoint)
    {
      if (decl == nullptr || decl->getIdentifier() == nullptr || decl->getName().empty())
      {
        return;
      }

      // Skip template parameters of generic template functions
      if (auto const* fd = dyn_cast<FunctionDecl>(decl->getDeclContext()); fd != nullptr)
      {
        if (fd->getTemplatedKind() != FunctionDecl::TK_NonTemplate)
        {
          return;
        }
      }

      // Conversion / factory functions are named after their input, not the type.
      if (isFunction && isConversionFunctionName(decl->getName()))
      {
        return;
      }

      auto const name = decl->getName();
      StringRef const category = isTimePoint ? StringRef{"time_point"} : StringRef{"duration"};

      // A redundant compound (elapsedDuration, nowTime) passes the suffix check
      // but stacks two time nouns, so reject it before the approval pass.
      if (StringRef const stem = redundantStem(name); !stem.empty())
      {
        diag(decl->getLocation(),
             "redundant %0 name '%1': stem '%2' is already a complete time noun; drop the trailing suffix")
          << category << name << stem;
        return;
      }

      if (isTimePoint ? isApprovedTimePointName(name) : isApprovedDurationName(name))
      {
        return;
      }

      auto const approvedList = isTimePoint ? timePointNounsList() : durationNounsList();

      if (isFunction)
      {
        diag(decl->getLocation(), "%0-returning function '%1' must end with an approved noun (%2)")
          << category << name << approvedList;
      }
      else if (isa<ParmVarDecl>(decl))
      {
        diag(decl->getLocation(), "%0 parameter '%1' must end with an approved noun (%2)")
          << category << name << approvedList;
      }
      else if (isa<FieldDecl>(decl))
      {
        diag(decl->getLocation(), "%0 field '%1' must end with an approved noun (%2)")
          << category << name << approvedList;
      }
      else
      {
        diag(decl->getLocation(), "%0 variable '%1' must end with an approved noun (%2)")
          << category << name << approvedList;
      }
    };

    auto const& nodes = result.Nodes;

    if (auto const* decl = nodes.getNodeAs<DeclaratorDecl>("durationDecl"); decl != nullptr)
    {
      processDecl(decl, false, false);
    }
    else if (auto const* func = nodes.getNodeAs<FunctionDecl>("durationFunc"); func != nullptr)
    {
      processDecl(func, true, false);
    }
    else if (auto const* decl = nodes.getNodeAs<DeclaratorDecl>("timePointDecl"); decl != nullptr)
    {
      processDecl(decl, false, true);
    }
    else if (auto const* func = nodes.getNodeAs<FunctionDecl>("timePointFunc"); func != nullptr)
    {
      processDecl(func, true, true);
    }
  }
} // namespace clang::tidy::readability
