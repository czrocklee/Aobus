// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    rt::TrackFieldRawValue rawValueFromEditValue(TrackFieldEditValue const& editValue)
    {
      if (auto const* text = std::get_if<std::string>(&editValue); text != nullptr)
      {
        return rt::TrackFieldRawValue{std::in_place_type<std::string>, *text};
      }

      if (auto const* number = std::get_if<std::uint16_t>(&editValue); number != nullptr)
      {
        return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, *number};
      }

      return rt::TrackFieldRawValue{};
    }

    TrackFieldEditValue editValueFromRawValue(rt::TrackFieldRawValue const& rawValue)
    {
      if (auto const* text = std::get_if<std::string>(&rawValue); text != nullptr)
      {
        return TrackFieldEditValue{std::in_place_type<std::string>, *text};
      }

      if (auto const* number = std::get_if<std::uint16_t>(&rawValue); number != nullptr)
      {
        return TrackFieldEditValue{std::in_place_type<std::uint16_t>, *number};
      }

      return TrackFieldEditValue{};
    }

    bool isFieldDirty(TrackPropertiesFormFieldState const& state)
    {
      auto patch = rt::MetadataPatch{};
      return writeTrackPropertiesFormEdit(patch, state, state.currentEditValue);
    }
  } // namespace

  void TrackPropertiesFormModel::addField(rt::TrackField field, bool editable)
  {
    _fields.push_back(TrackPropertiesFormFieldState{.field = field, .editable = editable});
  }

  void TrackPropertiesFormModel::clear()
  {
    _fields.clear();
  }

  void TrackPropertiesFormModel::loadFirstTrackField(rt::TrackField field, rt::TrackFieldRawValue rawValue)
  {
    if (auto* const state = findField(field); state != nullptr)
    {
      *state = makeTrackPropertiesFormFieldState(field, std::move(rawValue), state->editable);
    }
  }

  bool TrackPropertiesFormModel::mergeTrackField(rt::TrackField field, rt::TrackFieldRawValue const& rawValue)
  {
    if (auto* const state = findField(field); state != nullptr)
    {
      return mergeTrackPropertiesFormFieldState(*state, rawValue);
    }

    return false;
  }

  void TrackPropertiesFormModel::setEditValue(rt::TrackField field, TrackFieldEditValue editValue)
  {
    if (auto* const state = findField(field); state != nullptr)
    {
      state->currentEditValue = std::move(editValue);
    }
  }

  TrackPropertiesFormRowView TrackPropertiesFormModel::rowView(rt::TrackField field) const
  {
    if (auto const* const state = findField(field); state != nullptr)
    {
      return TrackPropertiesFormRowView{
        .field = state->field,
        .text = state->mixed ? std::string{kMultipleTrackValuesText}
                             : formatTrackFieldRawValue(state->field, state->originalRawValue),
        .mixed = state->mixed,
        .editable = state->editable,
        .dirty = isFieldDirty(*state),
      };
    }

    return TrackPropertiesFormRowView{.field = field, .mixed = false, .editable = false, .dirty = false};
  }

  bool TrackPropertiesFormModel::canSave() const
  {
    return std::ranges::any_of(_fields, isFieldDirty);
  }

  rt::MetadataPatch TrackPropertiesFormModel::buildPatch() const
  {
    auto patch = rt::MetadataPatch{};

    for (auto const& state : _fields)
    {
      std::ignore = writeTrackPropertiesFormEdit(patch, state, state.currentEditValue);
    }

    return patch;
  }

  TrackPropertiesFormFieldState* TrackPropertiesFormModel::findField(rt::TrackField field)
  {
    auto const iter = std::ranges::find_if(
      _fields, [field](TrackPropertiesFormFieldState const& state) { return state.field == field; });

    if (iter == _fields.end())
    {
      return nullptr;
    }

    return &*iter;
  }

  TrackPropertiesFormFieldState const* TrackPropertiesFormModel::findField(rt::TrackField field) const
  {
    auto const iter = std::ranges::find_if(
      _fields, [field](TrackPropertiesFormFieldState const& state) { return state.field == field; });

    if (iter == _fields.end())
    {
      return nullptr;
    }

    return &*iter;
  }

  TrackPropertiesFormFieldState makeTrackPropertiesFormFieldState(rt::TrackField field,
                                                                  rt::TrackFieldRawValue rawValue,
                                                                  bool editable)
  {
    auto currentEditValue = editValueFromRawValue(rawValue);
    return TrackPropertiesFormFieldState{
      .field = field,
      .originalRawValue = std::move(rawValue),
      .currentEditValue = std::move(currentEditValue),
      .mixed = false,
      .editable = editable,
    };
  }

  bool mergeTrackPropertiesFormFieldState(TrackPropertiesFormFieldState& state, rt::TrackFieldRawValue const& rawValue)
  {
    if (state.mixed || rawValue == state.originalRawValue)
    {
      return false;
    }

    state.mixed = true;
    return true;
  }

  bool writeTrackPropertiesFormEdit(rt::MetadataPatch& patch,
                                    TrackPropertiesFormFieldState const& state,
                                    TrackFieldEditValue const& editValue)
  {
    if (state.mixed || !state.editable || !canWriteTrackFieldPatch(state.field))
    {
      return false;
    }

    if (rawValueFromEditValue(editValue) == state.originalRawValue)
    {
      return false;
    }

    return writeTrackFieldPatch(patch, state.field, editValue);
  }
} // namespace ao::uimodel
