// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

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
    bool editMatchesOriginal(TrackPropertiesFormFieldState const& state)
    {
      if (auto const* text = std::get_if<std::string>(&state.currentEditValue); text != nullptr)
      {
        auto const* original = std::get_if<std::string>(&state.originalRawValue);
        return original != nullptr && *text == *original;
      }

      if (auto const* number = std::get_if<std::uint16_t>(&state.currentEditValue); number != nullptr)
      {
        auto const* original = std::get_if<std::uint16_t>(&state.originalRawValue);
        return original != nullptr && *number == *original;
      }

      return std::holds_alternative<std::monostate>(state.originalRawValue);
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

    TrackPropertiesFormFieldState makeTrackPropertiesFormFieldState(rt::TrackField field,
                                                                    rt::TrackFieldRawValue rawValue,
                                                                    bool const editable)
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

    bool mergeTrackPropertiesFormFieldState(TrackPropertiesFormFieldState& state,
                                            rt::TrackFieldRawValue const& rawValue)
    {
      if (state.mixed || rawValue == state.originalRawValue)
      {
        return false;
      }

      state.mixed = true;
      return true;
    }

    bool writeTrackPropertiesFormEdit(rt::MetadataPatch& patch, TrackPropertiesFormFieldState const& state)
    {
      if (!state.editable || !canWriteTrackFieldPatch(state.field) || (state.mixed && !state.explicitReplacement))
      {
        return false;
      }

      if (!state.mixed && editMatchesOriginal(state))
      {
        return false;
      }

      return writeTrackFieldPatch(patch, state.field, state.currentEditValue);
    }

    bool hasFieldChange(TrackPropertiesFormFieldState const& state)
    {
      auto patch = rt::MetadataPatch{};
      return writeTrackPropertiesFormEdit(patch, state);
    }
  } // namespace

  TrackPropertiesFormModel::TrackPropertiesFormModel(i18n::MessageCatalog textCatalog)
    : _textCatalog{std::move(textCatalog)}
  {
  }

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
      state->explicitReplacement = false;
    }
  }

  void TrackPropertiesFormModel::setExplicitFieldEdit(rt::TrackField field, TrackFieldEditValue value)
  {
    if (auto* const state = findField(field); state != nullptr)
    {
      state->currentEditValue = std::move(value);
      state->explicitReplacement = true;
    }
  }

  TrackPropertiesFormRowView TrackPropertiesFormModel::rowView(rt::TrackField field) const
  {
    if (auto const* const state = findField(field); state != nullptr)
    {
      return TrackPropertiesFormRowView{
        .field = state->field,
        .text = state->mixed ? std::string{i18n::requiredText(_textCatalog, i18n::MessageId::TrackMultipleValues)}
                             : formatTrackFieldRawValue(_textCatalog, state->field, state->originalRawValue),
        .mixed = state->mixed,
        .editable = state->editable,
      };
    }

    return TrackPropertiesFormRowView{.field = field, .mixed = false, .editable = false};
  }

  bool TrackPropertiesFormModel::canSave() const
  {
    return std::ranges::any_of(_fields, hasFieldChange);
  }

  rt::MetadataPatch TrackPropertiesFormModel::buildPatch() const
  {
    auto patch = rt::MetadataPatch{};

    for (auto const& state : _fields)
    {
      std::ignore = writeTrackPropertiesFormEdit(patch, state);
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
} // namespace ao::uimodel
