// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../detail/CompletionVocabulary.h"
#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Serializer.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    void appendSystemVariableItems(std::vector<CompletionItem>& items,
                                   query::VariableType type,
                                   std::string_view prefix,
                                   std::size_t limit)
    {
      auto const matches = query::completeQueryVariable(type, prefix);

      for (auto const& match : matches)
      {
        if (items.size() >= limit)
        {
          return;
        }

        auto text = std::string{};
        text.push_back(query::variablePrefix(match.type));
        text += match.canonicalName;

        items.push_back(CompletionItem{
          .displayText = text,
          .insertText = text,
          .detail = match.kind == query::QueryVariableCompletionMatchKind::ExactAlias ? "alias" : "field",
          .rank = static_cast<std::uint32_t>(items.size()),
        });
      }
    }

    void appendUserVariableItems(std::vector<CompletionItem>& items,
                                 query::VariableType type,
                                 std::span<VocabularyEntry const> vocabulary,
                                 std::string_view prefix,
                                 std::size_t limit)
    {
      appendVocabularyCompletionItems(
        items,
        vocabulary,
        prefix,
        limit,
        [type](VocabularyEntry const& entry)
        {
          auto const insertText = query::serialize(query::VariableExpression{.type = type, .name = entry.value});

          return CompletionItem{
            .displayText = insertText,
            .insertText = insertText,
            .detail = completionFrequencyDetail(entry.frequency),
          };
        });
    }

    std::optional<TrackField> trackFieldForQueryField(query::Field field)
    {
      switch (field)
      {
        case query::Field::ArtistId: return TrackField::Artist;
        case query::Field::AlbumId: return TrackField::Album;
        case query::Field::AlbumArtistId: return TrackField::AlbumArtist;
        case query::Field::GenreId: return TrackField::Genre;
        case query::Field::ComposerId: return TrackField::Composer;
        case query::Field::ConductorId: return TrackField::Conductor;
        case query::Field::EnsembleId: return TrackField::Ensemble;
        case query::Field::WorkId: return TrackField::Work;
        case query::Field::MovementId: return TrackField::Movement;
        case query::Field::SoloistId: return TrackField::Soloist;
        default: return std::nullopt;
      }
    }

    std::string insertionForOperator(std::string_view op)
    {
      if (op == "?")
      {
        return "?";
      }

      auto text = std::string{" "};
      text += op;
      text += " ";
      return text;
    }

    std::string insertionForStringConstant(std::string_view value)
    {
      return query::serialize(query::ConstantExpression{std::string{value}});
    }

    void appendOperatorItems(std::vector<CompletionItem>& items,
                             query::Field field,
                             std::string_view prefix,
                             std::size_t limit)
    {
      for (auto const op : query::completeQueryOperator(field, prefix))
      {
        if (items.size() >= limit)
        {
          return;
        }

        items.push_back(CompletionItem{
          .displayText = std::string{op},
          .insertText = insertionForOperator(op),
          .detail = "operator",
          .rank = static_cast<std::uint32_t>(items.size()),
        });
      }
    }

    void appendLogicalOperatorItems(std::vector<CompletionItem>& items, std::string_view prefix, std::size_t limit)
    {
      for (auto const op : query::completeQueryLogicalOperator(prefix))
      {
        if (items.size() >= limit)
        {
          return;
        }

        items.push_back(CompletionItem{
          .displayText = std::string{op},
          .insertText = insertionForOperator(op),
          .detail = "logical operator",
          .rank = static_cast<std::uint32_t>(items.size()),
        });
      }
    }

    void appendValueItems(std::vector<CompletionItem>& items,
                          CompletionService& vocabulary,
                          query::Field field,
                          std::string_view prefix,
                          std::size_t limit)
    {
      auto optTrackField = trackFieldForQueryField(field);

      if (!optTrackField)
      {
        return;
      }

      appendVocabularyCompletionItems(items,
                                      vocabulary.valuesFor(*optTrackField),
                                      prefix,
                                      limit,
                                      [](VocabularyEntry const& entry)
                                      {
                                        return CompletionItem{
                                          .displayText = entry.value,
                                          .insertText = insertionForStringConstant(entry.value),
                                          .detail = completionFrequencyDetail(entry.frequency),
                                        };
                                      });
    }

    struct AppendItemsVisitor final
    {
      std::vector<CompletionItem>& items;
      CompletionService& vocabulary;
      std::size_t limit = 0;

      void operator()(query::QueryCompletionToken const& token) const
      {
        switch (token.type)
        {
          case query::VariableType::Metadata:
          case query::VariableType::Property: appendSystemVariableItems(items, token.type, token.prefix, limit); break;
          case query::VariableType::Tag:
            appendUserVariableItems(items, token.type, vocabulary.tags(), token.prefix, limit);
            break;
          case query::VariableType::Custom:
            appendUserVariableItems(items, token.type, vocabulary.customKeys(), token.prefix, limit);
            break;
        }
      }

      void operator()(query::QueryOperatorCompletionContext const& context) const
      {
        appendOperatorItems(items, context.field, context.replacement.prefix, limit);
      }

      void operator()(query::QueryValueCompletionContext const& context) const
      {
        appendValueItems(items, vocabulary, context.field, context.replacement.prefix, limit);
      }

      void operator()(query::QueryLogicalOperatorCompletionContext const& context) const
      {
        appendLogicalOperatorItems(items, context.replacement.prefix, limit);
      }
    };

    std::pair<std::size_t, std::size_t> replacementRange(query::QueryCompletionContext const& context)
    {
      return std::visit(
        [](auto const& value) -> std::pair<std::size_t, std::size_t>
        {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, query::QueryCompletionToken>)
          {
            return {value.replaceBegin, value.replaceEnd};
          }
          else
          {
            return {value.replacement.replaceBegin, value.replacement.replaceEnd};
          }
        },
        context);
    }
  } // namespace

  QueryExpressionCompleter::QueryExpressionCompleter(CompletionService& vocabulary)
    : _vocabulary{vocabulary}
  {
  }

  std::optional<CompletionResult> QueryExpressionCompleter::complete(std::string_view text,
                                                                     std::size_t cursor,
                                                                     std::size_t limit)
  {
    auto optContext = query::analyzeCompletionContext(text, cursor);

    if (!optContext || limit == 0)
    {
      return std::nullopt;
    }

    auto items = std::vector<CompletionItem>{};
    items.reserve(std::min(limit, kCompletionResultLimit));

    std::visit(AppendItemsVisitor{.items = items, .vocabulary = _vocabulary, .limit = limit}, *optContext);

    if (items.empty())
    {
      return std::nullopt;
    }

    auto const [replaceBegin, replaceEnd] = replacementRange(*optContext);
    return CompletionResult{
      .replaceBegin = replaceBegin,
      .replaceEnd = replaceEnd,
      .items = std::move(items),
    };
  }
} // namespace ao::rt
