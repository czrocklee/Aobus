// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionVocabulary.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

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
    auto const matches = rt::selectCompletionVocabularyEntries(vocabulary, prefix, limit);
    auto suggestions = std::vector<std::string>{};
    suggestions.reserve(matches.size());

    for (auto const* const entry : matches)
    {
      suggestions.push_back(entry->value);
    }

    return suggestions;
  }

  bool canPresentTrackProperties(std::span<TrackId const> const selection) noexcept
  {
    return !selection.empty();
  }

  bool needsCustomMetadataValueUpdate(bool const existed,
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
