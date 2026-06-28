// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::string_view groupKeyName(rt::TrackGroupKey key)
    {
      switch (key)
      {
        case rt::TrackGroupKey::None: return "None";
        case rt::TrackGroupKey::Artist: return "Artist";
        case rt::TrackGroupKey::Album: return "Album";
        case rt::TrackGroupKey::AlbumArtist: return "Album Artist";
        case rt::TrackGroupKey::Genre: return "Genre";
        case rt::TrackGroupKey::Composer: return "Composer";
        case rt::TrackGroupKey::Work: return "Work";
        case rt::TrackGroupKey::Year: return "Year";
      }

      return "None";
    }
  } // namespace

  CustomPresentationEditorModel::CustomPresentationEditorModel()
    : _groupOptions{createGroupOptions()}
    , _sortFieldOptions{createSortFieldOptions()}
    , _visibleFieldOptions{createVisibleFieldOptions()}
  {
  }

  CustomPresentationEditorModel::CustomPresentationEditorModel(rt::TrackPresentationSpec const& spec,
                                                               std::string_view label)
    : CustomPresentationEditorModel{}
  {
    populate(spec, label);
  }

  void CustomPresentationEditorModel::populate(rt::TrackPresentationSpec const& spec, std::string_view label)
  {
    _label = std::string{label};
    _groupKey = spec.groupBy;
    _sortTerms = spec.sortBy;
    _visibleFields = spec.visibleFields;
  }

  void CustomPresentationEditorModel::setLabel(std::string_view label)
  {
    _label = std::string{label};
  }

  void CustomPresentationEditorModel::setGroupKey(rt::TrackGroupKey key)
  {
    _groupKey = key;
  }

  bool CustomPresentationEditorModel::setGroupKeyByOptionIndex(std::size_t optionIndex)
  {
    if (optionIndex >= _groupOptions.size())
    {
      return false;
    }

    _groupKey = _groupOptions[optionIndex].key;
    return true;
  }

  std::optional<std::size_t> CustomPresentationEditorModel::groupKeyOptionIndex() const
  {
    auto const it = std::ranges::find(_groupOptions, _groupKey, &TrackGroupKeyOption::key);

    if (it == _groupOptions.end())
    {
      return std::nullopt;
    }

    return static_cast<std::size_t>(std::ranges::distance(_groupOptions.begin(), it));
  }

  void CustomPresentationEditorModel::addSortTerm()
  {
    _sortTerms.push_back({rt::TrackSortField::Title, true});
  }

  bool CustomPresentationEditorModel::removeSortTerm(std::size_t index)
  {
    if (index >= _sortTerms.size())
    {
      return false;
    }

    _sortTerms.erase(_sortTerms.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
  }

  bool CustomPresentationEditorModel::moveSortTermUp(std::size_t index)
  {
    if (index == 0 || index >= _sortTerms.size())
    {
      return false;
    }

    std::swap(_sortTerms[index], _sortTerms[index - 1]);
    return true;
  }

  bool CustomPresentationEditorModel::moveSortTermDown(std::size_t index)
  {
    if (index + 1 >= _sortTerms.size())
    {
      return false;
    }

    std::swap(_sortTerms[index], _sortTerms[index + 1]);
    return true;
  }

  bool CustomPresentationEditorModel::setSortFieldByOptionIndex(std::size_t termIndex, std::size_t optionIndex)
  {
    if (termIndex >= _sortTerms.size() || optionIndex >= _sortFieldOptions.size())
    {
      return false;
    }

    _sortTerms[termIndex].field = _sortFieldOptions[optionIndex].field;
    return true;
  }

  bool CustomPresentationEditorModel::setSortAscending(std::size_t termIndex, bool ascending)
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
    auto const it = std::ranges::find(_sortFieldOptions, field, &TrackSortFieldOption::field);

    if (it == _sortFieldOptions.end())
    {
      return std::nullopt;
    }

    return static_cast<std::size_t>(std::ranges::distance(_sortFieldOptions.begin(), it));
  }

  void CustomPresentationEditorModel::addVisibleField()
  {
    _visibleFields.push_back(rt::TrackField::Title);
  }

  bool CustomPresentationEditorModel::removeVisibleField(std::size_t index)
  {
    if (index >= _visibleFields.size())
    {
      return false;
    }

    _visibleFields.erase(_visibleFields.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
  }

  bool CustomPresentationEditorModel::moveVisibleFieldUp(std::size_t index)
  {
    if (index == 0 || index >= _visibleFields.size())
    {
      return false;
    }

    std::swap(_visibleFields[index], _visibleFields[index - 1]);
    return true;
  }

  bool CustomPresentationEditorModel::moveVisibleFieldDown(std::size_t index)
  {
    if (index + 1 >= _visibleFields.size())
    {
      return false;
    }

    std::swap(_visibleFields[index], _visibleFields[index + 1]);
    return true;
  }

  bool CustomPresentationEditorModel::setVisibleFieldByOptionIndex(std::size_t fieldIndex, std::size_t optionIndex)
  {
    if (fieldIndex >= _visibleFields.size() || optionIndex >= _visibleFieldOptions.size())
    {
      return false;
    }

    _visibleFields[fieldIndex] = _visibleFieldOptions[optionIndex].field;
    return true;
  }

  std::optional<std::size_t> CustomPresentationEditorModel::optionIndexForVisibleField(rt::TrackField field) const
  {
    auto const it = std::ranges::find(_visibleFieldOptions, field, &TrackVisibleFieldOption::field);

    if (it == _visibleFieldOptions.end())
    {
      return std::nullopt;
    }

    return static_cast<std::size_t>(std::ranges::distance(_visibleFieldOptions.begin(), it));
  }

  rt::CustomTrackPresentationPreset CustomPresentationEditorModel::collectState(std::string_view generatedId) const
  {
    auto state = rt::CustomTrackPresentationPreset{};
    state.spec.id = std::string{generatedId};
    state.label = _label;
    state.spec.groupBy = _groupKey;
    state.spec.sortBy = _sortTerms;
    state.spec.visibleFields = _visibleFields;
    return state;
  }

  std::vector<TrackGroupKeyOption> CustomPresentationEditorModel::createGroupOptions()
  {
    auto const keys = std::vector{
      rt::TrackGroupKey::None,
      rt::TrackGroupKey::Artist,
      rt::TrackGroupKey::Album,
      rt::TrackGroupKey::AlbumArtist,
      rt::TrackGroupKey::Genre,
      rt::TrackGroupKey::Composer,
      rt::TrackGroupKey::Work,
      rt::TrackGroupKey::Year,
    };

    auto options = std::vector<TrackGroupKeyOption>{};
    options.reserve(keys.size());

    for (auto const key : keys)
    {
      options.push_back({.key = key, .label = std::string{groupKeyName(key)}});
    }

    return options;
  }

  std::vector<TrackSortFieldOption> CustomPresentationEditorModel::createSortFieldOptions()
  {
    auto options = std::vector<TrackSortFieldOption>{};
    auto const defs = rt::trackFieldDefinitions();

    for (std::size_t idx = 0; idx < rt::kTrackSortFieldCount; ++idx)
    {
      auto const sortField = static_cast<rt::TrackSortField>(idx);

      for (auto const& def : defs)
      {
        if (def.optSortField == sortField)
        {
          options.push_back({.field = sortField, .label = std::string{def.label}});
          break;
        }
      }
    }

    return options;
  }

  std::vector<TrackVisibleFieldOption> CustomPresentationEditorModel::createVisibleFieldOptions()
  {
    auto options = std::vector<TrackVisibleFieldOption>{};

    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (def.presentable)
      {
        options.push_back({.field = def.field, .label = std::string{def.label}});
      }
    }

    return options;
  }
} // namespace ao::uimodel
