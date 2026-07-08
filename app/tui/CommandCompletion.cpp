// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletion.h"

#include "ShellInteractionModel.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionText.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::size_t argumentBegin(std::string_view const draft, std::string_view const commandPrefix)
    {
      return std::min(commandPrefix.size(), draft.size());
    }

    bool looksLikeExpression(std::string_view const value)
    {
      auto const first = value.find_first_not_of(" \t");

      if (first == std::string_view::npos)
      {
        return false;
      }

      switch (value[first])
      {
        case '$':
        case '@':
        case '#':
        case '%':
        case '(':
        case '!': return true;
        default: return false;
      }
    }

    std::string_view commandDisplayText(std::string_view prefix)
    {
      if (!prefix.empty() && prefix.back() == ' ')
      {
        prefix.remove_suffix(1);
      }

      return prefix;
    }

    bool appendItem(std::vector<rt::CompletionItem>& items,
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
        .detail = std::move(detail),
        .rank = static_cast<std::uint32_t>(items.size()),
      });
      return true;
    }

    void appendCommandItems(std::vector<rt::CompletionItem>& items,
                            std::string_view const prefix,
                            std::size_t const limit)
    {
      for (auto const& spec : commandPrefixSpecs())
      {
        if (items.size() >= limit)
        {
          return;
        }

        if (auto const text = commandDisplayText(spec.prefix); rt::startsWithCompletionPrefixInsensitive(text, prefix))
        {
          if (!appendItem(items, limit, "/" + std::string{text}, std::string{spec.prefix}, std::string{spec.detail}))
          {
            return;
          }
        }
      }

      for (auto const& spec : commandAliasSpecs())
      {
        if (items.size() >= limit)
        {
          return;
        }

        if (rt::startsWithCompletionPrefixInsensitive(spec.alias, prefix))
        {
          if (!appendItem(
                items, limit, "/" + std::string{spec.alias}, std::string{spec.alias}, std::string{spec.detail}))
          {
            return;
          }
        }
      }
    }

    void appendPresentationItems(std::vector<rt::CompletionItem>& items,
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
          if (!appendItem(items, limit, preset.spec.id, preset.spec.id, std::string{preset.label}))
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
          if (!appendItem(items, limit, preset.spec.id, preset.spec.id, preset.label))
          {
            return;
          }
        }
      }
    }

    void appendQuickFilterItems(std::vector<rt::CompletionItem>& items,
                                CommandCompletionContext const& context,
                                std::string_view const prefix,
                                std::size_t const limit)
    {
      for (auto const& artist : context.artists)
      {
        if (items.size() >= limit)
        {
          return;
        }

        if (rt::startsWithCompletionPrefixInsensitive(artist.value, prefix))
        {
          if (!appendItem(items, limit, artist.value, artist.value, "artist"))
          {
            return;
          }
        }
      }

      for (auto const& list : context.lists)
      {
        if (items.size() >= limit)
        {
          return;
        }

        if (!list.completionText.empty() && rt::startsWithCompletionPrefixInsensitive(list.completionText, prefix))
        {
          if (!appendItem(items, limit, list.completionText, list.completionText, "list"))
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

    std::optional<rt::CompletionResult> completeExpression(CommandCompletionContext const& context,
                                                           std::string_view const expression,
                                                           std::size_t const offset,
                                                           std::size_t const limit)
    {
      if (!context.expressionCompleter)
      {
        return std::nullopt;
      }

      auto optResult = context.expressionCompleter(expression, expression.size(), limit);

      if (!optResult)
      {
        return std::nullopt;
      }

      optResult->replaceBegin += offset;
      optResult->replaceEnd += offset;
      return optResult;
    }
  } // namespace

  std::optional<rt::CompletionResult> completeCommandDraft(std::string_view const draft,
                                                           CommandCompletionContext const& context,
                                                           std::size_t const limit)
  {
    auto items = std::vector<rt::CompletionItem>{};
    items.reserve(limit);

    for (auto const& spec : commandPrefixSpecs())
    {
      if (rt::startsWithCompletionPrefixInsensitive(draft, spec.prefix))
      {
        auto const replaceBegin = argumentBegin(draft, spec.prefix);
        auto const argumentPrefix = draft.substr(replaceBegin);

        if (spec.action == CommandAction::SetPresentation)
        {
          appendPresentationItems(items, context, argumentPrefix, limit);
          return buildResult(replaceBegin, draft.size(), std::move(items));
        }

        if (looksLikeExpression(argumentPrefix))
        {
          if (auto optResult = completeExpression(context, argumentPrefix, replaceBegin, limit); optResult)
          {
            return optResult;
          }
        }

        appendQuickFilterItems(items, context, argumentPrefix, limit);
        return buildResult(replaceBegin, draft.size(), std::move(items));
      }
    }

    if (draft.find_first_of(" \t") == std::string_view::npos)
    {
      appendCommandItems(items, draft, limit);

      if (draft.empty())
      {
        return buildResult(0, 0, std::move(items));
      }
    }

    if (looksLikeExpression(draft))
    {
      if (auto optResult = completeExpression(context, draft, 0, limit); optResult)
      {
        return optResult;
      }
    }

    appendQuickFilterItems(items, context, draft, limit);
    return buildResult(0, draft.size(), std::move(items));
  }
} // namespace ao::tui
