// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  CustomPresentationEditorModel::CustomPresentationEditorModel(i18n::MessageCatalog const& textCatalog)
    : _groupOptions{makeGroupOptions(textCatalog)}
    , _sortFieldOptions{makeSortFieldOptions(textCatalog)}
    , _visibleFieldOptions{makeVisibleFieldOptions(textCatalog)}
  {
  }

  CustomPresentationEditorModel::CustomPresentationEditorModel(i18n::MessageCatalog const& textCatalog,
                                                               rt::TrackPresentationSpec const& spec,
                                                               std::string_view label)
    : CustomPresentationEditorModel{textCatalog}
  {
    populate(spec, label);
  }

  void CustomPresentationEditorModel::populate(rt::TrackPresentationSpec const& spec, std::string_view label)
  {
    _label = std::string{label};
    _optBasePresetId = spec.presetId;
    _groupKey = spec.groupBy;
    auto sortTerms = spec.effectiveSortBy();

    if (spec.optColumnSort &&
        rt::trackGroupSortBy(_groupKey, sortTerms) != rt::trackGroupSortBy(_groupKey, spec.sortBy))
    {
      // Only a group-key override can change section order. That key is not
      // compared within a section, so keep the base sort in this case.
      sortTerms = spec.sortBy;
    }

    _sortTerms.clear();
    _sortTerms.reserve(sortTerms.size());

    for (auto const& term : sortTerms)
    {
      if (!std::ranges::contains(_sortTerms, term.field, &rt::TrackSortTerm::field))
      {
        _sortTerms.push_back(term);
      }
    }

    _visibleFields = spec.visibleFields;
  }

  void CustomPresentationEditorModel::setLabel(std::string_view label)
  {
    _label = std::string{label};
  }

  bool CustomPresentationEditorModel::trySetGroupKeyByOptionIndex(std::size_t optionIndex)
  {
    if (optionIndex >= _groupOptions.size())
    {
      return false;
    }

    _groupKey = _groupOptions[optionIndex].value;
    return true;
  }

  std::optional<std::size_t> CustomPresentationEditorModel::groupKeyOptionIndex() const
  {
    return optionIndexOf(_groupOptions, _groupKey);
  }

  bool CustomPresentationEditorModel::tryAddSortTerm()
  {
    auto const isUnused = [this](rt::TrackSortField field)
    { return !std::ranges::contains(_sortTerms, field, &rt::TrackSortTerm::field); };
    auto field = rt::TrackSortField::Title;

    if (!isUnused(field))
    {
      auto const option = std::ranges::find_if(_sortFieldOptions, isUnused, &TrackSortFieldOption::value);

      if (option == _sortFieldOptions.end())
      {
        return false;
      }

      field = option->value;
    }

    _sortTerms.push_back({.field = field, .ascending = true});
    return true;
  }

  bool CustomPresentationEditorModel::tryRemoveSortTerm(std::size_t index)
  {
    return tryEraseElementAt(_sortTerms, index);
  }

  bool CustomPresentationEditorModel::tryMoveSortTermUp(std::size_t index)
  {
    return tryMoveElementUp(_sortTerms, index);
  }

  bool CustomPresentationEditorModel::tryMoveSortTermDown(std::size_t index)
  {
    return tryMoveElementDown(_sortTerms, index);
  }

  bool CustomPresentationEditorModel::trySetSortFieldByOptionIndex(std::size_t termIndex, std::size_t optionIndex)
  {
    if (termIndex >= _sortTerms.size() || optionIndex >= _sortFieldOptions.size())
    {
      return false;
    }

    auto const field = _sortFieldOptions[optionIndex].value;

    if (field != _sortTerms[termIndex].field && std::ranges::contains(_sortTerms, field, &rt::TrackSortTerm::field))
    {
      return false;
    }

    _sortTerms[termIndex].field = field;
    return true;
  }

  bool CustomPresentationEditorModel::trySetSortAscending(std::size_t termIndex, bool ascending)
  {
    if (termIndex >= _sortTerms.size())
    {
      return false;
    }

    _sortTerms[termIndex].ascending = ascending;
    return true;
  }

  std::optional<std::size_t> CustomPresentationEditorModel::optionIndexForSortField(rt::TrackSortField field) const
  {
    return optionIndexOf(_sortFieldOptions, field);
  }

  void CustomPresentationEditorModel::addVisibleField()
  {
    _visibleFields.push_back(rt::TrackField::Title);
  }

  bool CustomPresentationEditorModel::tryRemoveVisibleField(std::size_t index)
  {
    // A presentation with no visible columns would render nothing, so the last
    // field is not removable.
    return _visibleFields.size() != 1 && tryEraseElementAt(_visibleFields, index);
  }

  bool CustomPresentationEditorModel::tryMoveVisibleFieldUp(std::size_t index)
  {
    return tryMoveElementUp(_visibleFields, index);
  }

  bool CustomPresentationEditorModel::tryMoveVisibleFieldDown(std::size_t index)
  {
    return tryMoveElementDown(_visibleFields, index);
  }

  bool CustomPresentationEditorModel::trySetVisibleFieldByOptionIndex(std::size_t fieldIndex, std::size_t optionIndex)
  {
    if (fieldIndex >= _visibleFields.size() || optionIndex >= _visibleFieldOptions.size())
    {
      return false;
    }

    _visibleFields[fieldIndex] = _visibleFieldOptions[optionIndex].value;
    return true;
  }

  std::optional<std::size_t> CustomPresentationEditorModel::optionIndexForVisibleField(rt::TrackField field) const
  {
    return optionIndexOf(_visibleFieldOptions, field);
  }

  rt::CustomTrackPresentationPreset CustomPresentationEditorModel::collectState(rt::CustomPresetId const id) const
  {
    auto state = rt::CustomTrackPresentationPreset{};
    state.label = _label;
    state.optBasePresetId = _optBasePresetId;
    state.spec.presetId = id;
    state.spec.groupBy = _groupKey;
    state.spec.sortBy = _sortTerms;
    state.spec.visibleFields = _visibleFields;
    state.spec = rt::normalizeTrackPresentationSpec(state.spec);
    return state;
  }

  std::vector<TrackGroupKeyOption> CustomPresentationEditorModel::makeGroupOptions(
    i18n::MessageCatalog const& textCatalog)
  {
    auto const keys = std::vector{
      rt::TrackGroupKey::None,
      rt::TrackGroupKey::Artist,
      rt::TrackGroupKey::Album,
      rt::TrackGroupKey::AlbumArtist,
      rt::TrackGroupKey::Genre,
      rt::TrackGroupKey::Composer,
      rt::TrackGroupKey::Conductor,
      rt::TrackGroupKey::Ensemble,
      rt::TrackGroupKey::Work,
      rt::TrackGroupKey::Year,
    };

    auto options = std::vector<TrackGroupKeyOption>{};
    options.reserve(keys.size());

    for (auto const key : keys)
    {
      options.push_back({.value = key, .label = std::string{trackGroupKeyLabel(textCatalog, key)}});
    }

    return options;
  }

  std::vector<TrackSortFieldOption> CustomPresentationEditorModel::makeSortFieldOptions(
    i18n::MessageCatalog const& textCatalog)
  {
    auto options = std::vector<TrackSortFieldOption>{};
    options.reserve(rt::kTrackSortFieldCount);

    for (std::size_t index = 0; index < rt::kTrackSortFieldCount; ++index)
    {
      auto const sortField = static_cast<rt::TrackSortField>(index);

      if (auto const label = trackSortFieldLabel(textCatalog, sortField); !label.empty())
      {
        options.push_back({.value = sortField, .label = std::string{label}});
      }
    }

    return options;
  }

  std::vector<TrackVisibleFieldOption> CustomPresentationEditorModel::makeVisibleFieldOptions(
    i18n::MessageCatalog const& textCatalog)
  {
    auto options = std::vector<TrackVisibleFieldOption>{};

    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (def.presentable)
      {
        options.push_back({.value = def.field, .label = std::string{trackFieldLabel(textCatalog, def.field)}});
      }
    }

    return options;
  }
} // namespace ao::uimodel
