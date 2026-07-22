// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>

#include <string>
#include <vector>

namespace ao::uimodel
{
  struct TrackPropertiesFormFieldState final
  {
    rt::TrackField field = rt::TrackField::Title;
    rt::TrackFieldRawValue originalRawValue{};
    TrackFieldEditValue currentEditValue{};
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
    void addField(rt::TrackField field, bool editable);
    void clear();

    void loadFirstTrackField(rt::TrackField field, rt::TrackFieldRawValue rawValue);
    bool mergeTrackField(rt::TrackField field, rt::TrackFieldRawValue const& rawValue);
    void setEditValue(rt::TrackField field, TrackFieldEditValue editValue);

    TrackPropertiesFormRowView rowView(rt::TrackField field) const;
    bool canSave() const;
    rt::MetadataPatch buildPatch() const;

  private:
    TrackPropertiesFormFieldState* findField(rt::TrackField field);
    TrackPropertiesFormFieldState const* findField(rt::TrackField field) const;

    std::vector<TrackPropertiesFormFieldState> _fields;
  };

  TrackPropertiesFormFieldState makeTrackPropertiesFormFieldState(rt::TrackField field,
                                                                  rt::TrackFieldRawValue rawValue,
                                                                  bool editable = true);
  bool mergeTrackPropertiesFormFieldState(TrackPropertiesFormFieldState& state, rt::TrackFieldRawValue const& rawValue);
  bool writeTrackPropertiesFormEdit(rt::MetadataPatch& patch,
                                    TrackPropertiesFormFieldState const& state,
                                    TrackFieldEditValue const& editValue);
} // namespace ao::uimodel
