// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/presentation/PresentationText.h>

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
    _groupKey = spec.groupBy;
    _sortTerms = spec.sortBy;
    _visibleFields = spec.visibleFields;
  }

  void CustomPresentationEditorModel::setLabel(std::string_view label)
  {
    _label = std::string{label};
  }

  bool CustomPresentationEditorModel::setGroupKeyByOptionIndex(std::size_t optionIndex)
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

  void CustomPresentationEditorModel::addSortTerm()
  {
    _sortTerms.push_back({.field = rt::TrackSortField::Title, .ascending = true});
  }

  bool CustomPresentationEditorModel::removeSortTerm(std::size_t index)
  {
    return eraseElementAt(_sortTerms, index);
  }

  bool CustomPresentationEditorModel::moveSortTermUp(std::size_t index)
  {
    return moveElementUp(_sortTerms, index);
  }

  bool CustomPresentationEditorModel::moveSortTermDown(std::size_t index)
  {
    return moveElementDown(_sortTerms, index);
  }

  bool CustomPresentationEditorModel::setSortFieldByOptionIndex(std::size_t termIndex, std::size_t optionIndex)
  {
    if (termIndex >= _sortTerms.size() || optionIndex >= _sortFieldOptions.size())
    {
      return false;
    }

    _sortTerms[termIndex].field = _sortFieldOptions[optionIndex].value;
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
    return optionIndexOf(_sortFieldOptions, field);
  }

  void CustomPresentationEditorModel::addVisibleField()
  {
    _visibleFields.push_back(rt::TrackField::Title);
  }

  bool CustomPresentationEditorModel::removeVisibleField(std::size_t index)
  {
    // A presentation with no visible columns would render nothing, so the last
    // field is not removable.
    return _visibleFields.size() != 1 && eraseElementAt(_visibleFields, index);
  }

  bool CustomPresentationEditorModel::moveVisibleFieldUp(std::size_t index)
  {
    return moveElementUp(_visibleFields, index);
  }

  bool CustomPresentationEditorModel::moveVisibleFieldDown(std::size_t index)
  {
    return moveElementDown(_visibleFields, index);
  }

  bool CustomPresentationEditorModel::setVisibleFieldByOptionIndex(std::size_t fieldIndex, std::size_t optionIndex)
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
    auto const defs = rt::trackFieldDefinitions();

    for (std::size_t index = 0; index < rt::kTrackSortFieldCount; ++index)
    {
      auto const sortField = static_cast<rt::TrackSortField>(index);

      for (auto const& def : defs)
      {
        if (def.optSortField == sortField)
        {
          options.push_back({.value = sortField, .label = std::string{trackFieldLabel(textCatalog, def.field)}});
          break;
        }
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
