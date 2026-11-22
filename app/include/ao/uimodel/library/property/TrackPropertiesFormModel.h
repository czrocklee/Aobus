// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  class LibrarySnapshot;
}

namespace ao::uimodel
{
  struct TrackPropertiesFormSpec;

  struct TrackPropertiesFormFieldState final
  {
    enum class EditIntent : std::uint8_t
    {
      Ordinary,
      ExplicitReplacement,
      ExplicitClear,
    };

    rt::TrackField field = rt::TrackField::Title;
    rt::TrackFieldRawValue originalRawValue{};
    TrackFieldEditValue currentEditValue{};
    EditIntent editIntent = EditIntent::Ordinary;
    bool mixed = false;
    bool editable = false;
  };

  struct TrackPropertiesFormRowView final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::string text = {};
    bool mixed = false;
    bool editable = false;
  };

  struct TrackSegmentDisplayRow final
  {
    std::string label{};
    std::string text{};
  };

  // End is exclusive. Integer frame coordinates retain sub-millisecond boundaries.
  std::vector<TrackSegmentDisplayRow> formatTrackSegmentRows(i18n::MessageCatalog const& textCatalog,
                                                             std::uint16_t ordinal,
                                                             std::uint64_t startFrame,
                                                             std::uint64_t frameCount,
                                                             std::uint32_t sampleRate);

  class TrackPropertiesFormModel final
  {
  public:
    /// Starts unbound unless given the occurrence count for a manually assembled baseline.
    explicit TrackPropertiesFormModel(i18n::MessageCatalog textCatalog, std::size_t targetCount = 0);

    /// Captured target occurrences, including duplicates. Zero means the form is unbound.
    std::size_t targetCount() const noexcept;
    /// Resets the baseline and drafts, leaving the form unbound.
    void clear();

    /// Stages scalar user input, even for mixed fields. Absence withdraws the edit in a batch.
    /// Does nothing when unbound.
    void setFieldInput(rt::TrackField field, TrackFieldEditValue value);
    /// Withdraws the pending edit, preserving the captured value or mixed state.
    void restoreFieldEdit(rt::TrackField field);
    /// Stages the field's typed absence for Save. Does nothing when unbound.
    void clearFieldEdit(rt::TrackField field);

    // Manual baseline assembly; snapshot-backed forms use loadTrackPropertiesFormBaseline.
    void addField(rt::TrackField field, bool editable);
    void loadFirstTrackField(rt::TrackField field, rt::TrackFieldRawValue rawValue);
    bool tryMergeTrackField(rt::TrackField field, rt::TrackFieldRawValue const& rawValue);

    // Low-level typed edits bypass target-count and batch-absence policy. Use the input, restore,
    // and clear operations above for user-facing edits.
    /// Stages an edit that preserves mixed fields, even after an explicit replacement.
    void setEditValue(rt::TrackField field, TrackFieldEditValue editValue);
    /// Stages an edit that can replace a mixed field, including with typed absence.
    void setExplicitFieldEdit(rt::TrackField field, TrackFieldEditValue value);

    // Credits baseline is captured with ordinary fields and the owner's binding.
    std::optional<rt::CreditReplacement> const& pendingCredits() const noexcept;
    TrackCreditSections creditSections() const;
    Result<> beginCreditsEdit(std::bitset<library::kCreditKindCount> kinds);
    TrackCreditsEditorModel& creditsEditor() noexcept;
    TrackCreditsEditorModel const& creditsEditor() const noexcept;
    Result<> acceptCreditsEdit();
    void cancelCreditsEdit();

    /// The captured baseline, independent of pending edits.
    TrackPropertiesFormRowView rowView(rt::TrackField field) const;
    std::vector<TrackSegmentDisplayRow> const& segmentRows() const noexcept;
    /// True when the named scalar would contribute to the patch, irrespective of other draft fields.
    bool hasFieldChange(rt::TrackField field) const;
    /// Accepted scalar and Credits edits, even while a child editor blocks submission.
    bool hasEffectiveChanges() const;
    bool canSave() const;
    rt::MetadataPatch buildPatch() const;

  private:
    friend Result<> loadTrackPropertiesFormBaseline(rt::LibrarySnapshot const& snapshot,
                                                    std::span<TrackId const> targetIds,
                                                    TrackPropertiesFormSpec const& spec,
                                                    TrackPropertiesFormModel& form);

    TrackPropertiesFormFieldState* findField(rt::TrackField field);
    TrackPropertiesFormFieldState const* findField(rt::TrackField field) const;

    i18n::MessageCatalog _textCatalog;
    std::size_t _targetCount;
    std::vector<TrackPropertiesFormFieldState> _fields;
    TrackCreditSections _capturedCredits;
    std::vector<TrackSegmentDisplayRow> _segmentRows;
    std::optional<rt::CreditReplacement> _optPendingCredits;
    TrackCreditsEditorModel _creditsEditor;
  };

  /**
   * Replaces @p form with one standard-field baseline read from @p snapshot.
   *
   * The target sequence must be non-empty and every occurrence must name an
   * existing track. Validation finishes before the form is changed; duplicate
   * targets retain their input order and participate in aggregation.
   */
  Result<> loadTrackPropertiesFormBaseline(rt::LibrarySnapshot const& snapshot,
                                           std::span<TrackId const> targetIds,
                                           TrackPropertiesFormSpec const& spec,
                                           TrackPropertiesFormModel& form);
} // namespace ao::uimodel
