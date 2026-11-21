// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionVocabulary.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui
{
  TrackPropertyRowProjection projectTrackPropertyRow(uimodel::TrackPropertiesFormRow const& row,
                                                     uimodel::TrackPropertiesFormRowView const& view)
  {
    using Kind = uimodel::TrackPropertiesFormEditorKind;
    auto const controlKind = rt::creditKindForTrackField(row.field) ? Kind::ReadonlyText : row.editorKind;
    auto const placeholderOnly = view.mixed && controlKind != Kind::ReadonlyText;
    return TrackPropertyRowProjection{
      .field = row.field,
      .label = row.label,
      .text = placeholderOnly ? std::string{} : view.text,
      .placeholder = placeholderOnly ? view.text : std::string{},
      .controlKind = controlKind,
      .enabled = view.editable && controlKind != Kind::ReadonlyText,
    };
  }

  Result<> synchronizeTrackPropertyInput(uimodel::TrackPropertiesFormModel& form,
                                         TrackPropertyRowProjection const& row,
                                         std::string_view const text,
                                         bool const clear)
  {
    if (!row.enabled)
    {
      return {};
    }

    if (clear)
    {
      form.clearFieldEdit(row.field);
    }
    else if (text == row.text)
    {
      // Hydration, including a mixed placeholder, is never input.
      form.restoreFieldEdit(row.field);
    }
    else
    {
      auto valueRes = uimodel::parseTrackPropertiesFieldInput(row.controlKind, text);

      if (!valueRes)
      {
        form.restoreFieldEdit(row.field);
        return std::unexpected{valueRes.error()};
      }

      form.setFieldInput(row.field, std::move(*valueRes));
    }

    return {};
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

  bool canSubmitTrackProperties(uimodel::TrackPropertiesFormModel const& form, bool const auxiliaryDirty)
  {
    return !form.creditsEditor().isEditing() && (form.canSave() || auxiliaryDirty);
  }

  bool needsCustomMetadataValueUpdate(bool const existed,
                                      std::optional<std::string> const& optOriginalValue,
                                      std::string_view const value) noexcept
  {
    return !existed || !optOriginalValue || *optOriginalValue != value;
  }

  TrackPropertiesCommitState projectTrackPropertiesCommitState(rt::AuthoringStatus const status,
                                                               bool const bindingCurrent) noexcept
  {
    switch (status)
    {
      case rt::AuthoringStatus::Applied:
      case rt::AuthoringStatus::NoOp: return TrackPropertiesCommitState::Accepted;
      case rt::AuthoringStatus::Busy:
        return bindingCurrent ? TrackPropertiesCommitState::Busy : TrackPropertiesCommitState::Stale;
      case rt::AuthoringStatus::Stale: return TrackPropertiesCommitState::Stale;
      case rt::AuthoringStatus::Unavailable: return TrackPropertiesCommitState::Unavailable;
    }

    AO_FATAL("Unknown track-authoring status");
  }

  TrackPropertiesRetryFeedback projectTrackPropertiesRetryFeedback(TrackPropertiesRetryFeedback const previous,
                                                                   bool const bindingCurrent,
                                                                   TrackPropertiesRetryDraft const draft) noexcept
  {
    if (!bindingCurrent || previous == TrackPropertiesRetryFeedback::None ||
        draft == TrackPropertiesRetryDraft::Unchanged)
    {
      return TrackPropertiesRetryFeedback::None;
    }

    return draft == TrackPropertiesRetryDraft::SubmitReady ? TrackPropertiesRetryFeedback::Visible
                                                           : TrackPropertiesRetryFeedback::Deferred;
  }
} // namespace ao::winui
