// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletion.h"

#include "Command.h"
#include "ShellText.h"
#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionText.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/utility/UnicodeText.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::string_view commandDisplayText(std::string_view prefix)
    {
      if (!prefix.empty() && prefix.back() == ' ')
      {
        prefix.remove_suffix(1);
      }

      return prefix;
    }

    bool tryAppendItem(std::vector<rt::CompletionItem>& items,
                       std::size_t const limit,
                       std::string displayText,
                       std::string insertText,
                       std::string detail)
    {
      if (items.size() >= limit)
      {
        return false;
      }

      items.push_back(rt::CompletionItem{
        .displayText = std::move(displayText),
        .insertText = std::move(insertText),
        .detail = rt::CompletionDetail::makeResolvedText(std::move(detail)),
        .rank = static_cast<std::uint32_t>(items.size()),
      });
      return true;
    }

    enum class MatchRank : std::uint8_t
    {
      Exact,
      Prefix,
      Substring,
      Subsequence,
      None,
    };

    std::string commandSearchKey(std::string_view const value)
    {
      auto keyRes = utility::makeUtf8CaselessKey(value);
      AO_INVARIANT(keyRes, "Validated command text failed Unicode case folding: {}", keyRes.error().message);
      return std::move(*keyRes);
    }

    MatchRank commandMatchRank(std::string_view const candidate, std::string_view const query)
    {
      auto const key = commandSearchKey(candidate);

      if (key == query || query.empty())
      {
        return MatchRank::Exact;
      }

      if (key.starts_with(query))
      {
        return MatchRank::Prefix;
      }

      if (key.contains(query))
      {
        return MatchRank::Substring;
      }

      // Byte subsequences are meaningful only for ASCII abbreviations. Localized
      // names use complete UTF-8 substring matches, never fragments of scalars.
      if (query.size() > 4 || query.contains(' ') ||
          std::ranges::any_of(query, [](unsigned char byte) { return byte > std::numeric_limits<signed char>::max(); }))
      {
        return MatchRank::None;
      }

      std::size_t position = 0;

      for (auto const letter : query)
      {
        position = key.find(letter, position);

        if (position == std::string::npos)
        {
          return MatchRank::None;
        }

        ++position;
      }

      return MatchRank::Subsequence;
    }

    void appendCommandItems(std::vector<rt::CompletionItem>& items,
                            i18n::MessageCatalog const& textCatalog,
                            std::string_view const query,
                            std::size_t const limit)
    {
      struct Candidate final
      {
        CommandAction action;
        rt::CompletionItem item;
        MatchRank rank;
      };

      auto candidates = std::vector<Candidate>{};
      auto const queryKey = commandSearchKey(query);

      auto append = [&](CommandAction action, std::string_view spelling, i18n::MessageId label)
      {
        auto const name = chromeText(textCatalog, label);
        auto const rank = std::min(commandMatchRank(spelling, queryKey), commandMatchRank(name, queryKey));
        auto const existing = std::ranges::find(candidates, action, &Candidate::action);

        if (existing != candidates.end())
        {
          existing->rank = std::min(existing->rank, rank);
          return;
        }

        candidates.push_back(Candidate{
          .action = action,
          .item = {.displayText = name,
                   .insertText = std::string{spelling},
                   .detail = rt::CompletionDetail::makeResolvedText(":" + std::string{commandDisplayText(spelling)})},
          .rank = rank});
      };

      for (auto const& spec : commandPrefixSpecs())
      {
        append(spec.action, spec.prefix, spec.detail);
      }

      for (auto const& spec : commandAliasSpecs())
      {
        append(spec.action, spec.alias, spec.detail);
      }

      std::ranges::stable_sort(candidates, {}, &Candidate::rank);

      auto const hasStrongMatch = !candidates.empty() && candidates.front().rank < MatchRank::Subsequence;

      for (auto& candidate : candidates)
      {
        if (candidate.rank == MatchRank::None || (hasStrongMatch && candidate.rank == MatchRank::Subsequence) ||
            items.size() >= limit)
        {
          break;
        }

        candidate.item.rank = static_cast<std::uint32_t>(items.size());
        items.push_back(std::move(candidate.item));
      }
    }

    void appendPresentationItems(std::vector<rt::CompletionItem>& items,
                                 i18n::MessageCatalog const& textCatalog,
                                 CommandCompletionContext const& context,
                                 std::string_view const prefix,
                                 std::size_t const limit)
    {
      for (auto const& preset : context.builtinPresentations)
      {
        if (items.size() >= limit)
        {
          return;
        }

        if (rt::startsWithCompletionPrefixInsensitive(preset.spec.id, prefix))
        {
          auto const optText = uimodel::builtinTrackPresentation(textCatalog, preset.spec.id);

          if (!tryAppendItem(
                items, limit, preset.spec.id, preset.spec.id, optText ? std::string{optText->label} : preset.spec.id))
          {
            return;
          }
        }
      }

      for (auto const& preset : context.customPresentations)
      {
        if (items.size() >= limit)
        {
          return;
        }

        if (rt::startsWithCompletionPrefixInsensitive(preset.spec.id, prefix))
        {
          if (!tryAppendItem(items, limit, preset.spec.id, preset.spec.id, preset.label))
          {
            return;
          }
        }
      }
    }

    std::optional<rt::CompletionResult> buildResult(std::size_t const replaceBegin,
                                                    std::size_t const replaceEnd,
                                                    std::vector<rt::CompletionItem> items)
    {
      if (items.empty())
      {
        return std::nullopt;
      }

      return rt::CompletionResult{
        .replaceBegin = replaceBegin,
        .replaceEnd = replaceEnd,
        .items = std::move(items),
      };
    }

    std::optional<rt::CompletionResult> completeFilter(CommandCompletionContext const& context,
                                                       std::string_view const filter,
                                                       std::size_t const offset,
                                                       std::size_t const cursor,
                                                       std::size_t const limit)
    {
      if (!context.filterCompleter)
      {
        return std::nullopt;
      }

      auto optResult = context.filterCompleter(filter, cursor, limit);

      if (!optResult)
      {
        return std::nullopt;
      }

      optResult->replaceBegin += offset;
      optResult->replaceEnd += offset;
      return optResult;
    }
  } // namespace

  std::optional<rt::CompletionResult> completeCommandDraft(i18n::MessageCatalog const& textCatalog,
                                                           std::string_view const draft,
                                                           std::size_t const cursor,
                                                           CommandCompletionContext const& context,
                                                           std::size_t const limit)
  {
    if (cursor > draft.size())
    {
      return std::nullopt;
    }

    auto items = std::vector<rt::CompletionItem>{};
    items.reserve(limit);

    for (auto const& spec : commandPrefixSpecs())
    {
      if (cursor >= spec.prefix.size() && rt::startsWithCompletionPrefixInsensitive(draft, spec.prefix))
      {
        auto const replaceBegin = spec.prefix.size();
        auto const argumentPrefix = draft.substr(replaceBegin);

        if (spec.action == CommandAction::SetPresentation)
        {
          appendPresentationItems(items, textCatalog, context, argumentPrefix.substr(0, cursor - replaceBegin), limit);
          return buildResult(replaceBegin, draft.size(), std::move(items));
        }

        return completeFilter(context, argumentPrefix, replaceBegin, cursor - replaceBegin, limit);
      }
    }

    appendCommandItems(items, textCatalog, draft, limit);

    if (!items.empty())
    {
      return buildResult(0, draft.size(), std::move(items));
    }

    return std::nullopt;
  }
} // namespace ao::tui
