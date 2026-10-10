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
    rt::TrackField field = rt::TrackField::Title;
    rt::TrackFieldRawValue originalRawValue{};
    TrackFieldEditValue currentEditValue{};
    bool explicitReplacement = false;
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

  class TrackPropertiesFormModel final
  {
  public:
    explicit TrackPropertiesFormModel(i18n::MessageCatalog textCatalog);

    void addField(rt::TrackField field, bool editable);
    void clear();

    void loadFirstTrackField(rt::TrackField field, rt::TrackFieldRawValue rawValue);
    bool tryMergeTrackField(rt::TrackField field, rt::TrackFieldRawValue const& rawValue);
    void setEditValue(rt::TrackField field, TrackFieldEditValue editValue);
    /// Replaces even a mixed baseline; a later setEditValue restores ordinary mixed-field preservation.
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
    std::vector<TrackPropertiesFormFieldState> _fields;
    TrackCreditSections _capturedCredits;
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
