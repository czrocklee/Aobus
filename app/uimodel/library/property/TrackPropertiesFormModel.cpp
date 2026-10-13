// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    bool matchesOriginalEdit(TrackPropertiesFormFieldState const& state)
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

      if (auto const* date = std::get_if<library::RecordingDate>(&state.currentEditValue); date != nullptr)
      {
        auto const* original = std::get_if<library::RecordingDate>(&state.originalRawValue);
        return original != nullptr ? *date == *original : !date->isPresent();
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

      if (auto const* date = std::get_if<library::RecordingDate>(&rawValue); date != nullptr)
      {
        return TrackFieldEditValue{*date};
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

    bool tryMergeTrackPropertiesFormFieldState(TrackPropertiesFormFieldState& state,
                                               rt::TrackFieldRawValue const& rawValue)
    {
      if (state.mixed || rawValue == state.originalRawValue)
      {
        return false;
      }

      state.mixed = true;
      return true;
    }

    bool tryWriteTrackPropertiesFormEdit(rt::MetadataPatch& patch, TrackPropertiesFormFieldState const& state)
    {
      if (!state.editable || !canWriteTrackFieldPatch(state.field) || (state.mixed && !state.explicitReplacement))
      {
        return false;
      }

      if (!state.mixed && matchesOriginalEdit(state))
      {
        return false;
      }

      return tryWriteTrackFieldPatch(patch, state.field, state.currentEditValue);
    }

    bool hasFieldChange(TrackPropertiesFormFieldState const& state)
    {
      auto patch = rt::MetadataPatch{};
      return tryWriteTrackPropertiesFormEdit(patch, state);
    }

    void addBaselineRows(TrackPropertiesFormModel& form, std::span<TrackPropertiesFormRow const> const rows)
    {
      for (auto const& row : rows)
      {
        form.addField(row.field, row.editorKind != TrackPropertiesFormEditorKind::ReadonlyText);
      }
    }
  } // namespace

  TrackPropertiesFormModel::TrackPropertiesFormModel(i18n::MessageCatalog textCatalog)
    : _textCatalog{std::move(textCatalog)}
  {
  }

  void TrackPropertiesFormModel::addField(rt::TrackField field, bool editable)
  {
    _fields.push_back(
      TrackPropertiesFormFieldState{.field = field, .editable = editable && canWriteTrackFieldPatch(field)});
  }

  void TrackPropertiesFormModel::clear()
  {
    _fields.clear();
    _capturedCredits = {};
    _optPendingCredits.reset();
    _creditsEditor.cancel();
  }

  void TrackPropertiesFormModel::loadFirstTrackField(rt::TrackField field, rt::TrackFieldRawValue rawValue)
  {
    if (auto* const state = findField(field); state != nullptr)
    {
      *state = makeTrackPropertiesFormFieldState(field, std::move(rawValue), state->editable);
    }
  }

  bool TrackPropertiesFormModel::tryMergeTrackField(rt::TrackField field, rt::TrackFieldRawValue const& rawValue)
  {
    if (auto* const state = findField(field); state != nullptr)
    {
      return tryMergeTrackPropertiesFormFieldState(*state, rawValue);
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

  std::optional<rt::CreditReplacement> const& TrackPropertiesFormModel::pendingCredits() const noexcept
  {
    return _optPendingCredits;
  }

  TrackCreditSections TrackPropertiesFormModel::creditSections() const
  {
    auto sections = _capturedCredits;

    if (_optPendingCredits)
    {
      auto entriesByKind = std::array<std::vector<library::Credit>, library::kCreditKindCount>{};

      for (auto const& entry : _optPendingCredits->entries)
      {
        entriesByKind[static_cast<std::size_t>(entry.kind)].push_back(entry);
      }

      for (std::size_t i = 0; i < sections.size(); ++i)
      {
        if (_optPendingCredits->kinds.test(i))
        {
          sections[i] = {.optValue = std::move(entriesByKind[i])};
        }
      }
    }

    return sections;
  }

  Result<> TrackPropertiesFormModel::beginCreditsEdit(std::bitset<library::kCreditKindCount> const kinds)
  {
    return _creditsEditor.begin(creditSections(), kinds);
  }

  TrackCreditsEditorModel& TrackPropertiesFormModel::creditsEditor() noexcept
  {
    return _creditsEditor;
  }

  TrackCreditsEditorModel const& TrackPropertiesFormModel::creditsEditor() const noexcept
  {
    return _creditsEditor;
  }

  Result<> TrackPropertiesFormModel::acceptCreditsEdit()
  {
    auto patchRes = _creditsEditor.buildCommitPatch();

    if (!patchRes)
    {
      return std::unexpected{patchRes.error()};
    }

    if (patchRes->optCredits)
    {
      auto replacement = std::move(*patchRes->optCredits);

      if (_optPendingCredits)
      {
        for (auto const& entry : _optPendingCredits->entries)
        {
          if (!replacement.kinds.test(static_cast<std::size_t>(entry.kind)))
          {
            replacement.entries.push_back(entry);
          }
        }

        replacement.kinds |= _optPendingCredits->kinds;
      }

      // Both accepted drafts are canonical already; restore only fixed kind grouping after overlay.
      auto entries = std::vector<library::Credit>{};
      entries.reserve(replacement.entries.size());

      for (std::size_t i = 0; i < library::kCreditKindCount; ++i)
      {
        for (auto& entry : replacement.entries)
        {
          if (entry.kind == static_cast<library::CreditKind>(i))
          {
            entries.push_back(std::move(entry));
          }
        }
      }

      replacement.entries = std::move(entries);
      auto const original = scopedTrackCreditsBaseline(_capturedCredits, replacement.kinds);

      if (!original.mixed && original.optValue && *original.optValue == replacement.entries)
      {
        _optPendingCredits.reset();
      }
      else
      {
        _optPendingCredits = std::move(replacement);
      }
    }

    _creditsEditor.cancel();
    return {};
  }

  void TrackPropertiesFormModel::cancelCreditsEdit()
  {
    _creditsEditor.cancel();
  }

  TrackPropertiesFormRowView TrackPropertiesFormModel::rowView(rt::TrackField field) const
  {
    if (auto const optKind = rt::creditKindForTrackField(field); optKind)
    {
      auto const& section = _capturedCredits[static_cast<std::size_t>(*optKind)];

      return {.field = field, .text = formatTrackCreditSectionSummary(_textCatalog, section), .mixed = section.mixed};
    }

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
    return !_creditsEditor.isEditing() && (_optPendingCredits || std::ranges::any_of(_fields, hasFieldChange));
  }

  rt::MetadataPatch TrackPropertiesFormModel::buildPatch() const
  {
    auto patch = rt::MetadataPatch{};

    if (_creditsEditor.isEditing())
    {
      return patch;
    }

    patch.optCredits = _optPendingCredits;

    for (auto const& state : _fields)
    {
      std::ignore = tryWriteTrackPropertiesFormEdit(patch, state);
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

  Result<> loadTrackPropertiesFormBaseline(rt::LibrarySnapshot const& snapshot,
                                           std::span<TrackId const> const targetIds,
                                           TrackPropertiesFormSpec const& spec,
                                           TrackPropertiesFormModel& form)
  {
    if (targetIds.empty())
    {
      return makeError(Error::Code::InvalidInput, "Track properties require at least one target");
    }

    for (auto const trackId : targetIds)
    {
      if (!snapshot.containsTrack(trackId))
      {
        return makeError(Error::Code::NotFound, "Track properties target not found");
      }
    }

    auto creditsRes = loadTrackCreditsEditorBaseline(snapshot, targetIds);

    if (!creditsRes)
    {
      return std::unexpected{creditsRes.error()};
    }

    auto baseline = TrackPropertiesFormModel{form._textCatalog};
    baseline._capturedCredits = std::move(*creditsRes);
    addBaselineRows(baseline, spec.metadataRows);
    addBaselineRows(baseline, spec.propertyRows);

    bool first = true;
    auto loadRows = [&](TrackId const trackId, std::span<TrackPropertiesFormRow const> const rows)
    {
      for (auto const& row : rows)
      {
        if (auto const* const state = baseline.findField(row.field); !first && state != nullptr && state->mixed)
        {
          continue;
        }

        if (auto rawValue = snapshot.trackField(trackId, row.field); first)
        {
          baseline.loadFirstTrackField(row.field, std::move(rawValue));
        }
        else
        {
          std::ignore = baseline.tryMergeTrackField(row.field, rawValue);
        }
      }
    };

    for (auto const trackId : targetIds)
    {
      loadRows(trackId, spec.metadataRows);
      loadRows(trackId, spec.propertyRows);
      first = false;
    }

    form = std::move(baseline);
    return {};
  }
} // namespace ao::uimodel
