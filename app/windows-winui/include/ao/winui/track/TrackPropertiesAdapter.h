// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
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
  enum class TrackPropertyControlKind : std::uint8_t
  {
    Text,
    Number,
    ReadonlyText,
  };

  enum class TrackPropertiesCommitState : std::uint8_t
  {
    Accepted,
    Busy,
    Stale,
    Unavailable,
  };

  struct TrackPropertyRowProjection final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::string label;
    std::string text;
    TrackPropertyControlKind controlKind = TrackPropertyControlKind::Text;
    bool mixed = false;
    bool enabled = false;

    bool operator==(TrackPropertyRowProjection const&) const = default;
  };

  TrackPropertyControlKind trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind kind) noexcept;
  TrackPropertyRowProjection projectTrackPropertyRow(uimodel::TrackPropertiesFormRow const& row,
                                                     uimodel::TrackPropertiesFormRowView const& view);
  Result<uimodel::TrackFieldEditValue> parseTrackPropertyEdit(TrackPropertyControlKind kind, std::string_view text);
  std::vector<std::string> trackPropertyVocabularySuggestions(std::span<rt::VocabularyEntry const> vocabulary,
                                                              std::string_view prefix,
                                                              std::size_t limit);
  bool canPresentTrackProperties(std::span<TrackId const> selection) noexcept;
  bool customMetadataValueNeedsUpdate(bool existed,
                                      std::optional<std::string> const& optOriginalValue,
                                      std::string_view value) noexcept;
  TrackPropertiesCommitState projectTrackPropertiesCommitState(rt::AuthoringStatus status) noexcept;
} // namespace ao::winui
