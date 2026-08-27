// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  // A choice offered by a dropdown: the value it selects plus its authored label.
  template<typename T>
  struct LabeledOption final
  {
    T value{};
    std::string label;
  };

  using TrackGroupKeyOption = LabeledOption<rt::TrackGroupKey>;
  using TrackSortFieldOption = LabeledOption<rt::TrackSortField>;
  using TrackVisibleFieldOption = LabeledOption<rt::TrackField>;

  class CustomPresentationEditorModel final
  {
  public:
    explicit CustomPresentationEditorModel(i18n::MessageCatalog const& textCatalog);
    CustomPresentationEditorModel(i18n::MessageCatalog const& textCatalog,
                                  rt::TrackPresentationSpec const& spec,
                                  std::string_view label);

    void populate(rt::TrackPresentationSpec const& spec, std::string_view label);

    std::string_view label() const noexcept { return _label; }
    void setLabel(std::string_view label);

    std::span<TrackGroupKeyOption const> groupOptions() const noexcept { return _groupOptions; }
    std::span<TrackSortFieldOption const> sortFieldOptions() const noexcept { return _sortFieldOptions; }
    std::span<TrackVisibleFieldOption const> visibleFieldOptions() const noexcept { return _visibleFieldOptions; }

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
    // The sort-term and visible-field lists offer the same reordering gestures,
    // and every dropdown resolves a current value back to its option index.

    template<typename T>
    static bool moveElementUp(std::vector<T>& elements, std::size_t index)
    {
      if (index == 0 || index >= elements.size())
      {
        return false;
      }

      std::swap(elements[index], elements[index - 1]);
      return true;
    }

    template<typename T>
    static bool moveElementDown(std::vector<T>& elements, std::size_t index)
    {
      if (index + 1 >= elements.size())
      {
        return false;
      }

      std::swap(elements[index], elements[index + 1]);
      return true;
    }

    template<typename T>
    static bool eraseElementAt(std::vector<T>& elements, std::size_t index)
    {
      if (index >= elements.size())
      {
        return false;
      }

      elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(index));
      return true;
    }

    template<typename T>
    static std::optional<std::size_t> optionIndexOf(std::vector<LabeledOption<T>> const& options, T value)
    {
      auto const iter = std::ranges::find(options, value, &LabeledOption<T>::value);

      if (iter == options.end())
      {
        return std::nullopt;
      }

      return static_cast<std::size_t>(std::ranges::distance(options.begin(), iter));
    }

    static std::vector<TrackGroupKeyOption> makeGroupOptions(i18n::MessageCatalog const& textCatalog);
    static std::vector<TrackSortFieldOption> makeSortFieldOptions(i18n::MessageCatalog const& textCatalog);
    static std::vector<TrackVisibleFieldOption> makeVisibleFieldOptions(i18n::MessageCatalog const& textCatalog);

    std::string _label;
    rt::TrackGroupKey _groupKey = rt::TrackGroupKey::None;
    std::vector<rt::TrackSortTerm> _sortTerms;
    std::vector<rt::TrackField> _visibleFields;
    std::vector<TrackGroupKeyOption> _groupOptions;
    std::vector<TrackSortFieldOption> _sortFieldOptions;
    std::vector<TrackVisibleFieldOption> _visibleFieldOptions;
  };
} // namespace ao::uimodel
