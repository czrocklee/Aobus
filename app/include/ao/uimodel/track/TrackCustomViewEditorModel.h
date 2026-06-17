// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::track
{
  struct TrackGroupKeyOption final
  {
    rt::TrackGroupKey key = rt::TrackGroupKey::None;
    std::string label;
  };

  struct TrackSortFieldOption final
  {
    rt::TrackSortField field = rt::TrackSortField::Title;
    std::string label;
  };

  struct TrackVisibleFieldOption final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::string label;
  };

  class TrackCustomViewEditorModel final
  {
  public:
    TrackCustomViewEditorModel();
    TrackCustomViewEditorModel(rt::TrackPresentationSpec const& spec, std::string_view label);

    void populate(rt::TrackPresentationSpec const& spec, std::string_view label);

    std::string_view label() const noexcept { return _label; }
    void setLabel(std::string_view label);

    std::span<TrackGroupKeyOption const> groupOptions() const noexcept { return _groupOptions; }
    std::span<TrackSortFieldOption const> sortFieldOptions() const noexcept { return _sortFieldOptions; }
    std::span<TrackVisibleFieldOption const> visibleFieldOptions() const noexcept { return _visibleFieldOptions; }

    rt::TrackGroupKey groupKey() const noexcept { return _groupKey; }
    void setGroupKey(rt::TrackGroupKey key);
    bool setGroupKeyByOptionIndex(std::size_t optionIndex);
    std::optional<std::size_t> groupKeyOptionIndex() const;

    std::span<rt::TrackSortTerm const> sortTerms() const noexcept { return _sortTerms; }
    void addSortTerm();
    bool removeSortTerm(std::size_t index);
    bool moveSortTermUp(std::size_t index);
    bool moveSortTermDown(std::size_t index);
    bool setSortFieldByOptionIndex(std::size_t termIndex, std::size_t optionIndex);
    bool setSortAscending(std::size_t termIndex, bool ascending);
    std::optional<std::size_t> optionIndexForSortField(rt::TrackSortField field) const;

    std::span<rt::TrackField const> visibleFields() const noexcept { return _visibleFields; }
    void addVisibleField();
    bool removeVisibleField(std::size_t index);
    bool moveVisibleFieldUp(std::size_t index);
    bool moveVisibleFieldDown(std::size_t index);
    bool setVisibleFieldByOptionIndex(std::size_t fieldIndex, std::size_t optionIndex);
    std::optional<std::size_t> optionIndexForVisibleField(rt::TrackField field) const;

    rt::CustomTrackPresentationPreset collectState(std::string_view generatedId) const;

  private:
    static std::vector<TrackGroupKeyOption> createGroupOptions();
    static std::vector<TrackSortFieldOption> createSortFieldOptions();
    static std::vector<TrackVisibleFieldOption> createVisibleFieldOptions();

    std::string _label;
    rt::TrackGroupKey _groupKey = rt::TrackGroupKey::None;
    std::vector<rt::TrackSortTerm> _sortTerms;
    std::vector<rt::TrackField> _visibleFields;
    std::vector<TrackGroupKeyOption> _groupOptions;
    std::vector<TrackSortFieldOption> _sortFieldOptions;
    std::vector<TrackVisibleFieldOption> _visibleFieldOptions;
  };
} // namespace ao::uimodel::track
