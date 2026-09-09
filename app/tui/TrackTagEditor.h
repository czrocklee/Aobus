// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MouseBindings.h"
#include "TextFieldModel.h"
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  /// Tag intents for a captured selection, with terminal search and selection state.
  class TrackTagEditor final
  {
  public:
    TrackTagEditor(i18n::MessageCatalog textCatalog,
                   std::size_t targetCount,
                   std::vector<std::pair<std::string, std::size_t>> tagCounts,
                   std::vector<std::string> tagSuggestions);

    bool isDirty() const noexcept;
    std::size_t additionCount() const noexcept;
    std::size_t removalCount() const noexcept;
    void appendChanges(std::vector<std::string>& additions, std::vector<std::string>& removals) const;
    bool tryDismissQuery(ftxui::Event const& event);
    void clearQuery();
    bool isCreatingTag() const noexcept { return _offersNewTag && _focusedTagRow >= _visibleTags.size(); }
    void handleEvent(ftxui::Event const& event);
    ftxui::Element render() const;

  private:
    enum class TagIntent : std::uint8_t
    {
      Preserve,
      AddToAll,
      RemoveFromAll,
    };

    struct TagRow final
    {
      std::string name{};
      std::string searchKey{};
      std::size_t originalCount = 0;
      TagIntent intent = TagIntent::Preserve;
      bool suggested = false;
    };

    bool isEffectiveTagEdit(TagRow const& tag) const noexcept;
    void moveTagRow(std::int32_t delta);
    std::optional<std::string> normalizedTagQuery() const;
    void refreshVisibleTags();
    void focusTag(std::size_t tagIndex);
    void cycleTagIntent(TagRow& tag) const noexcept;
    void commitFocusedTag();
    ftxui::Element renderTagCheckbox(TagRow const& tag) const;
    ftxui::Element renderTagStatus(TagRow const& tag, std::size_t total) const;
    ftxui::Element renderTagsList() const;

    i18n::MessageCatalog _textCatalog;
    std::size_t _targetCount;
    std::vector<TagRow> _tags{};
    TextFieldModel _tagQuery{};
    std::vector<std::size_t> _visibleTags{};
    std::size_t _focusedTagRow = 0;
    std::size_t _hiddenSuggestionCount = 0;
    bool _offersNewTag = false;
    mutable std::vector<ftxui::Box> _rowBoxes;
    mutable std::vector<std::size_t> _renderedTags;
    mutable std::string _renderedQuery;
    mutable ftxui::Box _listViewport = kEmptyMouseBox;
    mutable ftxui::Box _queryBox = kEmptyMouseBox;
    mutable ftxui::Box _queryTextBox = kEmptyMouseBox;
  };
} // namespace ao::tui
