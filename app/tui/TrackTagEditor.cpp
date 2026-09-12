// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackTagEditor.h"

#include "MouseBindings.h"
#include "SelectableList.h"
#include "SelectionNavigation.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;

    /// Width of the tag name column.
    constexpr std::int32_t kTagColumns = 32;
    /// Minimum width of the column carrying how many targets already carry a tag.
    constexpr std::int32_t kTagStatusColumns = 8;
    /**
     * @brief Suggested tags the list draws at once, matching the GTK tag editor.
     *
     * The cap is on the rows drawn, never on the vocabulary searched: a tag
     * ranked below it stays reachable by typing its name. Tags the draft
     * already marks are exempt, or marking one and then dropping the query
     * would hide the edit that was just made.
     */
    constexpr std::size_t kVisibleTagSuggestions = 50;

    std::string tagSearchKey(std::string_view const name)
    {
      auto keyRes = utility::makeUtf8CaselessKey(name);
      AO_INVARIANT(keyRes, "Validated tag text failed Unicode case folding: {}", keyRes.error().message);
      return std::move(*keyRes);
    }
  } // namespace

  TrackTagEditor::TrackTagEditor(i18n::MessageCatalog textCatalog,
                                 std::size_t const targetCount,
                                 std::vector<std::pair<std::string, std::size_t>> tagCounts,
                                 std::vector<std::string> tagSuggestions)
    : _textCatalog{std::move(textCatalog)}, _targetCount{targetCount}
  {
    _tags.reserve(tagCounts.size() + tagSuggestions.size());

    for (auto& [tag, count] : tagCounts)
    {
      auto searchKey = tagSearchKey(tag);
      _tags.push_back(TagRow{
        .name = std::move(tag),
        .searchKey = std::move(searchKey),
        .originalCount = count,
        .intent = TagIntent::Preserve,
        .suggested = false,
      });
    }

    for (auto& tag : tagSuggestions)
    {
      auto searchKey = tagSearchKey(tag);
      _tags.push_back(TagRow{
        .name = std::move(tag),
        .searchKey = std::move(searchKey),
        .originalCount = 0,
        .intent = TagIntent::Preserve,
        .suggested = true,
      });
    }

    refreshVisibleTags();
  }

  bool TrackTagEditor::isDirty() const noexcept
  {
    return std::ranges::any_of(_tags, [this](auto const& tag) { return isEffectiveTagEdit(tag); });
  }

  std::size_t TrackTagEditor::additionCount() const noexcept
  {
    return static_cast<std::size_t>(std::ranges::count_if(
      _tags, [this](auto const& tag) { return tag.intent == TagIntent::AddToAll && isEffectiveTagEdit(tag); }));
  }

  std::size_t TrackTagEditor::removalCount() const noexcept
  {
    return static_cast<std::size_t>(std::ranges::count_if(
      _tags, [this](auto const& tag) { return tag.intent == TagIntent::RemoveFromAll && isEffectiveTagEdit(tag); }));
  }

  void TrackTagEditor::appendChanges(std::vector<std::string>& additions, std::vector<std::string>& removals) const
  {
    for (auto const& tag : _tags)
    {
      if (isEffectiveTagEdit(tag))
      {
        (tag.intent == TagIntent::AddToAll ? additions : removals).push_back(tag.name);
      }
    }
  }

  bool TrackTagEditor::tryDismissQuery(ftxui::Event const& event)
  {
    if (event != ftxui::Event::Escape || _tagQuery.empty())
    {
      return false;
    }

    clearQuery();
    return true;
  }

  void TrackTagEditor::clearQuery()
  {
    _tagQuery.reset("");
    refreshVisibleTags();
  }

  void TrackTagEditor::activateFocusedTag()
  {
    std::size_t tagIndex = 0;

    if (_focusedTagRow < _visibleTags.size())
    {
      tagIndex = _visibleTags[_focusedTagRow];
      cycleTagIntent(_tags[tagIndex]);
    }
    else if (_offersNewTag)
    {
      auto optNormalized = normalizedTagQuery();

      if (!optNormalized || utility::trim(*optNormalized).empty())
      {
        return;
      }

      // _tags stays partitioned into the selection's own tags and then the
      // library suggestions, so a created tag joins the first group rather
      // than being buried under every suggestion the library has.
      auto const firstSuggested = std::ranges::find(_tags, true, &TagRow::suggested);
      auto searchKey = tagSearchKey(*optNormalized);
      auto const inserted = _tags.insert(firstSuggested,
                                         TagRow{
                                           .name = std::move(*optNormalized),
                                           .searchKey = std::move(searchKey),
                                           .originalCount = 0,
                                           .intent = TagIntent::AddToAll,
                                           .suggested = false,
                                         });
      tagIndex = static_cast<std::size_t>(std::distance(_tags.begin(), inserted));
    }
    else
    {
      return;
    }

    // The query has done its job. Dropping it puts the tag back among its
    // neighbours so the change is read in context, and leaves the field ready
    // for the next name.
    _tagQuery.reset("");
    refreshVisibleTags();
    focusTag(tagIndex);
  }

  void TrackTagEditor::createTagFromQuery()
  {
    if (_offersNewTag)
    {
      _focusedTagRow = _visibleTags.size();
      activateFocusedTag();
    }
  }

  void TrackTagEditor::handleEvent(ftxui::Event const& event)
  {
    if (event.is_mouse())
    {
      auto mouseEvent = event;

      if (auto const& mouse = mouseEvent.mouse();
          isLeftPress(mouse) && _renderedQuery == _tagQuery.value() && _renderedTags == _visibleTags)
      {
        if (containsMouse(_queryBox, mouse))
        {
          _tagQuery.tryMoveToCell(mouse.x - _queryTextBox.x_min);
        }
        else if (auto const optRow = mouseRowAt(_rowBoxes, mouse); optRow)
        {
          _focusedTagRow = *optRow;
          activateFocusedTag();
          _rowBoxes.clear();
        }
      }

      return;
    }

    if (event == ftxui::Event::ArrowUp)
    {
      moveTagRow(-1);
      return;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      moveTagRow(1);
      return;
    }

    if (event == ftxui::Event::PageUp)
    {
      moveTagRow(-navigationPageRows(_listViewport));
      return;
    }

    if (event == ftxui::Event::PageDown)
    {
      moveTagRow(navigationPageRows(_listViewport));
      return;
    }

    if (event == ftxui::Event::Return)
    {
      activateFocusedTag();
      return;
    }

    if (event == ftxui::Event::CtrlG)
    {
      if (_focusedTagRow < _visibleTags.size())
      {
        // Restoring puts a suggestion back under the display cap that its own
        // intent had exempted it from, so the rows are derived again and the
        // selection follows the tag rather than snapping to the first row.
        auto const tagIndex = _visibleTags[_focusedTagRow];
        _tags[tagIndex].intent = TagIntent::Preserve;
        refreshVisibleTags();
        focusTag(tagIndex);
      }

      return;
    }

    // Everything the list does not claim is text. The query owns the keyboard
    // because typing a name is the only way to reach a tag the display cap
    // leaves out, and it is what names a tag the library does not have yet.
    if (_tagQuery.tryApplyEvent(event))
    {
      refreshVisibleTags();
    }
  }

  bool TrackTagEditor::isQueryHit(ftxui::Mouse const& mouse) const
  {
    return containsMouse(_queryBox, mouse);
  }

  std::int32_t TrackTagEditor::visibleRowCount() const noexcept
  {
    return static_cast<std::int32_t>(_visibleTags.size()) + (_offersNewTag ? 1 : 0);
  }

  ftxui::Element TrackTagEditor::render(bool const queryFocused, std::optional<std::int32_t> const optColumns) const
  {
    using namespace ftxui;

    // Keep mouse hits tied to the query and results from this frame.
    _renderedQuery = _tagQuery.value();
    _renderedTags = _visibleTags;
    auto const& query = _tagQuery.value();
    auto queryInputPtr = textFieldValue(_tagQuery, &_queryTextBox, queryFocused);

    if (!query.empty())
    {
      queryInputPtr = std::move(queryInputPtr) | flex;
    }

    auto queryCells = Elements{text(" "), std::move(queryInputPtr)};

    if (query.empty())
    {
      // The caret occupies the placeholder's first cell, so the hint resumes
      // after it rather than sitting underneath it.
      queryCells.push_back(text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorTagsFilterHint)}) |
                           style::muted());
    }

    auto bodyElements = Elements{
      // Unbordered like every metadata field, so the caret is what says this
      // is an input and the rule below is the only thing dividing it from the
      // results it filters. A query wider than the modal scrolls to the caret
      // rather than shrinking every segment away from it.
      hbox(std::move(queryCells)) | ftxui::reflect(_queryBox),
      separator(),
      renderTagsList(optColumns),
    };

    // A truncated list would otherwise claim the library holds nothing further.
    if (_hiddenSuggestionCount > 0)
    {
      bodyElements.push_back(text(i18n::requiredFormat(
                               _textCatalog, MessageId::TuiEditorTagsMoreHidden, {{"count", _hiddenSuggestionCount}})) |
                             style::muted());
    }

    return vbox(std::move(bodyElements));
  }

  bool TrackTagEditor::isEffectiveTagEdit(TagRow const& tag) const noexcept
  {
    // Adding a tag every target already carries, or removing one none of them
    // carries, writes nothing. The footer, Apply, and the patch all have to
    // agree about that, so they all ask here.
    return (tag.intent == TagIntent::AddToAll && tag.originalCount < _targetCount) ||
           (tag.intent == TagIntent::RemoveFromAll && tag.originalCount > 0);
  }

  void TrackTagEditor::moveTagRow(std::int32_t const delta)
  {
    auto const rowCount = static_cast<std::int32_t>(_visibleTags.size()) + (_offersNewTag ? 1 : 0);

    if (rowCount == 0)
    {
      return;
    }

    auto const target = std::clamp(static_cast<std::int32_t>(_focusedTagRow) + delta, 0, rowCount - 1);
    _focusedTagRow = static_cast<std::size_t>(target);
  }

  std::optional<std::string> TrackTagEditor::normalizedTagQuery() const
  {
    auto normalizedRes = utility::normalizeUtf8Nfc(_tagQuery.value());

    if (!normalizedRes)
    {
      return std::nullopt;
    }

    return std::move(*normalizedRes);
  }

  void TrackTagEditor::refreshVisibleTags()
  {
    _visibleTags.clear();
    _hiddenSuggestionCount = 0;
    _offersNewTag = false;

    // Compare normalized caseless keys, but preserve the stored spelling when
    // accepting an existing tag and the normalized spelling when creating one.
    auto const optNormalizedQuery = normalizedTagQuery();
    auto const query = tagSearchKey(_tagQuery.value());
    std::size_t shownSuggestions = 0;
    bool exactMatch = false;

    for (std::size_t index = 0; index < _tags.size(); ++index)
    {
      auto const& tag = _tags[index];
      auto const matchesExactly = !query.empty() && tag.searchKey == query;

      if (matchesExactly)
      {
        exactMatch = true;
      }

      if (!query.empty() && !tag.searchKey.contains(query))
      {
        continue;
      }

      // Draft tags and exact matches stay reachable regardless of frequency.
      if (tag.suggested && tag.intent == TagIntent::Preserve && !matchesExactly)
      {
        if (shownSuggestions == kVisibleTagSuggestions)
        {
          ++_hiddenSuggestionCount;
          continue;
        }

        ++shownSuggestions;
      }

      _visibleTags.push_back(index);
    }

    // The normalized form is what a submission would write, so a query that
    // normalizes to nothing names no tag worth offering.
    if (!query.empty() && !exactMatch)
    {
      _offersNewTag = optNormalizedQuery && !utility::trim(*optNormalizedQuery).empty();
    }

    _focusedTagRow = 0;
  }

  void TrackTagEditor::focusTag(std::size_t const tagIndex)
  {
    auto const it = std::ranges::find(_visibleTags, tagIndex);
    _focusedTagRow = it == _visibleTags.end() ? 0 : static_cast<std::size_t>(std::distance(_visibleTags.begin(), it));
  }

  void TrackTagEditor::cycleTagIntent(TagRow& tag) const noexcept
  {
    // Adding a tag every target already carries writes nothing, and neither
    // does removing one none of them carries, so the cycle visits only the
    // intents that would reach storage. A mixed selection is the one case with
    // a real choice, and it is the one case with three stops.
    switch (auto const total = _targetCount; tag.intent)
    {
      case TagIntent::Preserve:
        tag.intent = tag.originalCount == total ? TagIntent::RemoveFromAll : TagIntent::AddToAll;
        break;
      case TagIntent::AddToAll:
        tag.intent = tag.originalCount == 0 ? TagIntent::Preserve : TagIntent::RemoveFromAll;
        break;
      case TagIntent::RemoveFromAll: tag.intent = TagIntent::Preserve; break;
    }
  }

  ftxui::Element TrackTagEditor::renderTagCheckbox(TagRow const& tag) const
  {
    using namespace ftxui;

    // The box shows what every target would carry after a submission, so a
    // pending intent moves it now and colours it to say the move is pending.
    if (tag.intent == TagIntent::AddToAll)
    {
      return text("[x] ") | style::success();
    }

    if (tag.intent == TagIntent::RemoveFromAll)
    {
      return text("[ ] ") | style::danger();
    }

    if (tag.originalCount == 0)
    {
      return text("[ ] ");
    }

    return text(tag.originalCount == _targetCount ? "[x] " : "[~] ");
  }

  ftxui::Element TrackTagEditor::renderTagStatus(TagRow const& tag, std::size_t const total) const
  {
    using namespace ftxui;

    // The box shows where the tag is going, which leaves the fraction as the
    // only place it says where the tag started. A tag every target carries or
    // none does has no starting point the box does not already show, so only a
    // partly carried one spends a column on it.
    auto const partlyCarried = tag.originalCount > 0 && tag.originalCount < total;
    auto countText = partlyCarried ? std::format("{}/{}", tag.originalCount, total) : std::string{};

    if (tag.intent == TagIntent::Preserve)
    {
      return text(std::move(countText));
    }

    auto const verbId =
      tag.intent == TagIntent::AddToAll ? MessageId::TuiEditorTagsStateAdd : MessageId::TuiEditorTagsStateRemove;
    auto verbPtr = text(std::string{i18n::requiredText(_textCatalog, verbId)});

    return hbox({
      text(fitCellText(countText, std::max(kTagStatusColumns, cellWidth(countText) + 1))),
      tag.intent == TagIntent::AddToAll ? std::move(verbPtr) | style::success() : std::move(verbPtr) | style::danger(),
    });
  }

  ftxui::Element TrackTagEditor::renderTagsList(std::optional<std::int32_t> const optColumns) const
  {
    using namespace ftxui;

    auto const total = _targetCount;
    constexpr std::int32_t kTagChromeColumns = 8;
    auto const statusColumns =
      std::max(kTagStatusColumns, cellWidth(std::format("{}/{}", total, total)) + 1) +
      std::max(cellWidth(i18n::requiredText(_textCatalog, i18n::MessageId::TuiEditorTagsStateAdd)),
               cellWidth(i18n::requiredText(_textCatalog, i18n::MessageId::TuiEditorTagsStateRemove)));
    constexpr std::int32_t kMinimumTagColumns = 8;
    auto const compact = optColumns && *optColumns < kMinimumTagColumns + statusColumns + kTagChromeColumns;
    auto const tagColumns =
      optColumns ? std::clamp(*optColumns - kTagChromeColumns - (compact ? 0 : statusColumns), 1, kTagColumns)
                 : kTagColumns;
    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_visibleTags.size() + (_offersNewTag ? 1 : 0));

    _rowBoxes.assign(_visibleTags.size() + (_offersNewTag ? 1 : 0), kEmptyMouseBox);

    auto const marker = [](bool const focused)
    { return text(focused ? "> " : "  ") | (focused ? bold : style::muted()); };

    for (std::size_t row = 0; row < _visibleTags.size(); ++row)
    {
      auto const& tag = _tags[_visibleTags[row]];

      rows.push_back(SelectableListRow{
        .elementPtr = hbox({
          marker(_focusedTagRow == row),
          renderTagCheckbox(tag),
          text(fitCellText(tag.name, tagColumns)),
          text(" "),
          compact ? text("") : renderTagStatus(tag, total),
        }),
        .box = &_rowBoxes[row],
      });
    }

    // Creation trails existing matches in result navigation. Query activation
    // can choose this offer directly without moving the result cursor.
    if (_offersNewTag)
    {
      auto const isFocused = _focusedTagRow == _visibleTags.size();
      auto newTagText =
        i18n::requiredFormat(_textCatalog, MessageId::TuiEditorTagsActionAddNew, {{"name", _tagQuery.value()}});

      rows.push_back(SelectableListRow{
        .elementPtr = hbox({
          marker(isFocused),
          text("    "),
          text(std::move(newTagText)) | bold,
        }),
        .box = &_rowBoxes.back(),
      });
    }

    // Every row is exactly one line, so the row index is already the line
    // coordinate selectableList wants.
    return selectableList(std::move(rows),
                          SelectableListOptions{
                            .focusRow = static_cast<std::int32_t>(_focusedTagRow),
                            .horizontalScroll = false,
                            .flex = true,
                            .viewportBox = &_listViewport,
                          });
  }
} // namespace ao::tui
