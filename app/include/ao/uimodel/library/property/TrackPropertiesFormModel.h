// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <string>
#include <vector>

namespace ao::uimodel
{
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
    bool mergeTrackField(rt::TrackField field, rt::TrackFieldRawValue const& rawValue);
    void setEditValue(rt::TrackField field, TrackFieldEditValue editValue);
    /// Replaces even a mixed baseline; a later setEditValue restores ordinary mixed-field preservation.
    void setExplicitFieldEdit(rt::TrackField field, TrackFieldEditValue value);

    /// The captured baseline, independent of pending edits.
    TrackPropertiesFormRowView rowView(rt::TrackField field) const;
    bool canSave() const;
    rt::MetadataPatch buildPatch() const;

  private:
    TrackPropertiesFormFieldState* findField(rt::TrackField field);
    TrackPropertiesFormFieldState const* findField(rt::TrackField field) const;

    i18n::MessageCatalog _textCatalog;
    std::vector<TrackPropertiesFormFieldState> _fields;
  };
} // namespace ao::uimodel
