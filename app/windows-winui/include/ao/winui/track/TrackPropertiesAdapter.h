// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  struct VocabularyEntry;
}

namespace ao::winui
{
  enum class TrackPropertiesCommitState : std::uint8_t
  {
    Accepted,
    Busy,
    Stale,
    Unavailable,
  };

  enum class TrackPropertiesRetryFeedback : std::uint8_t
  {
    None,
    Deferred,
    Visible,
  };

  enum class TrackPropertiesRetryDraft : std::uint8_t
  {
    // Valid input with no staged change. Invalid raw input remains Blocked even without a patch.
    Unchanged,
    Blocked,
    SubmitReady,
  };

  struct TrackPropertyRowProjection final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::string label;
    std::string text;
    std::string placeholder;
    uimodel::TrackPropertiesFormEditorKind controlKind = uimodel::TrackPropertiesFormEditorKind::Text;
    bool enabled = false;

    bool operator==(TrackPropertyRowProjection const&) const = default;
  };

  TrackPropertyRowProjection projectTrackPropertyRow(uimodel::TrackPropertiesFormRow const& row,
                                                     uimodel::TrackPropertiesFormRowView const& view);
  Result<> synchronizeTrackPropertyInput(uimodel::TrackPropertiesFormModel& form,
                                         TrackPropertyRowProjection const& row,
                                         std::string_view text,
                                         bool clear);
  std::vector<std::string> trackPropertyVocabularySuggestions(std::span<rt::VocabularyEntry const> vocabulary,
                                                              std::string_view prefix,
                                                              std::size_t limit);
  bool canPresentTrackProperties(std::span<TrackId const> selection) noexcept;
  bool canSubmitTrackProperties(uimodel::TrackPropertiesFormModel const& form, bool auxiliaryDirty);
  bool needsCustomMetadataValueUpdate(bool existed,
                                      std::optional<std::string> const& optOriginalValue,
                                      std::string_view value) noexcept;
  TrackPropertiesCommitState projectTrackPropertiesCommitState(rt::AuthoringStatus status,
                                                               bool bindingCurrent) noexcept;
  // A Busy completion seeds Deferred; only a submit-ready draft can offer a retry.
  TrackPropertiesRetryFeedback projectTrackPropertiesRetryFeedback(TrackPropertiesRetryFeedback previous,
                                                                   bool bindingCurrent,
                                                                   TrackPropertiesRetryDraft draft) noexcept;
} // namespace ao::winui
