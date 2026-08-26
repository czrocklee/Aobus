// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionText.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::winui
{
  TrackPropertyControlKind trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind const kind) noexcept
  {
    switch (kind)
    {
      case uimodel::TrackPropertiesFormEditorKind::Text: return TrackPropertyControlKind::Text;
      case uimodel::TrackPropertiesFormEditorKind::Number: return TrackPropertyControlKind::Number;
      case uimodel::TrackPropertiesFormEditorKind::ReadonlyText: return TrackPropertyControlKind::ReadonlyText;
    }

    AO_FATAL("Unknown track-property editor kind");
  }

  TrackPropertyRowProjection projectTrackPropertyRow(uimodel::TrackPropertiesFormRow const& row,
                                                     uimodel::TrackPropertiesFormRowView const& view)
  {
    auto const controlKind = trackPropertyControlKind(row.editorKind);
    return TrackPropertyRowProjection{
      .field = row.field,
      .label = row.label,
      .text = view.text,
      .controlKind = controlKind,
      .mixed = view.mixed,
      .enabled = view.editable && !view.mixed && controlKind != TrackPropertyControlKind::ReadonlyText,
    };
  }

  Result<uimodel::TrackFieldEditValue> parseTrackPropertyEdit(TrackPropertyControlKind const kind,
                                                              std::string_view const text)
  {
    switch (kind)
    {
      case TrackPropertyControlKind::Text: return uimodel::parseTextEditValue(text);
      case TrackPropertyControlKind::Number: return uimodel::parseUint16EditValue(text);
      case TrackPropertyControlKind::ReadonlyText:
        return makeError(Error::Code::InvalidState, "A read-only property cannot be edited.");
    }

    AO_FATAL("Unknown native track-property control kind");
  }

  std::vector<std::string> trackPropertyVocabularySuggestions(std::span<rt::VocabularyEntry const> const vocabulary,
                                                              std::string_view const prefix,
                                                              std::size_t const limit)
  {
    auto suggestions = std::vector<std::string>{};

    if (limit == 0)
    {
      return suggestions;
    }

    suggestions.reserve(std::min(limit, vocabulary.size()));
    auto wordMatches = std::vector<rt::VocabularyEntry const*>{};
    auto const optAliasPrefix = rt::makeCompletionAliasPrefixKey(prefix);

    for (auto const& entry : vocabulary)
    {
      auto const optMatchOffset = rt::findCompletionWordPrefixInsensitive(entry.value, prefix);

      if (optMatchOffset && *optMatchOffset == 0)
      {
        suggestions.push_back(entry.value);
      }
      else if (optMatchOffset)
      {
        wordMatches.push_back(&entry);
      }

      if (suggestions.size() >= limit)
      {
        return suggestions;
      }
    }

    for (auto const* const entry : wordMatches)
    {
      suggestions.push_back(entry->value);

      if (suggestions.size() >= limit)
      {
        return suggestions;
      }
    }

    if (!optAliasPrefix)
    {
      return suggestions;
    }

    for (auto const& entry : vocabulary)
    {
      if (entry.aliases.empty() ||
          std::ranges::none_of(
            entry.aliases, [&](std::string_view const alias) { return alias.starts_with(*optAliasPrefix); }) ||
          std::ranges::contains(suggestions, entry.value))
      {
        continue;
      }

      suggestions.push_back(entry.value);

      if (suggestions.size() >= limit)
      {
        break;
      }
    }

    return suggestions;
  }

  bool canPresentTrackProperties(std::span<TrackId const> const selection) noexcept
  {
    return !selection.empty();
  }

  bool customMetadataValueNeedsUpdate(bool const existed,
                                      std::optional<std::string> const& optOriginalValue,
                                      std::string_view const value) noexcept
  {
    return !existed || !optOriginalValue || *optOriginalValue != value;
  }

  TrackPropertiesCommitState projectTrackPropertiesCommitState(rt::AuthoringStatus const status) noexcept
  {
    switch (status)
    {
      case rt::AuthoringStatus::Applied:
      case rt::AuthoringStatus::NoOp: return TrackPropertiesCommitState::Accepted;
      case rt::AuthoringStatus::Busy: return TrackPropertiesCommitState::Busy;
      case rt::AuthoringStatus::Stale: return TrackPropertiesCommitState::Stale;
      case rt::AuthoringStatus::Unavailable: return TrackPropertiesCommitState::Unavailable;
    }

    AO_FATAL("Unknown track-authoring status");
  }
} // namespace ao::winui
