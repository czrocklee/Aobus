// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackPropertiesEditor.h"

#include "MouseBindings.h"
#include "SelectableList.h"
#include "SelectionNavigation.h"
#include "Style.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackMutation.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;
    /// Terminal lines one target occupies: its title, then its path.
    constexpr std::int32_t kTargetRowLines = 2;

    std::string countedText(i18n::MessageCatalog const& textCatalog, MessageId const id, std::size_t const count)
    {
      return i18n::requiredFormat(textCatalog, id, {{"count", count}});
    }
  } // namespace

  TrackPropertiesEditor::TrackPropertiesEditor(i18n::MessageCatalog textCatalog,
                                               TrackEditorPreparation preparation,
                                               CompletionProvider completionProvider)
    : _textCatalog{std::move(textCatalog)}
    , _targets{std::move(preparation.targets)}
    , _metadataEditor{_textCatalog, _targets.size(), std::move(preparation.baseline), std::move(completionProvider)}
    , _tagEditor{_textCatalog, _targets.size(), std::move(preparation.tagCounts), std::move(preparation.tagSuggestions)}
  {
  }

  bool TrackPropertiesEditor::isDirty() const noexcept
  {
    return _metadataEditor.isDirty() || _tagEditor.isDirty();
  }

  void TrackPropertiesEditor::setStatus(TrackEditorStatus const status, std::string diagnostic)
  {
    _status = status;
    _diagnostic = std::move(diagnostic);

    // A question about a draft is meaningless once the write it guarded is
    // already running or already refused.
    if (_status != TrackEditorStatus::Ready)
    {
      _metadataEditor.closeCompletion();
      _confirmingDiscard = false;
      _confirmingReload = false;
    }
  }

  TrackEditorRequest TrackPropertiesEditor::takeRequest() noexcept
  {
    return std::exchange(_request, TrackEditorRequest::None);
  }

  bool TrackPropertiesEditor::canApply() const noexcept
  {
    return _status == TrackEditorStatus::Ready && isDirty() && !_metadataEditor.hasInvalidFields();
  }

  TrackEditorPatchSummary TrackPropertiesEditor::patchSummary() const noexcept
  {
    return TrackEditorPatchSummary{
      .fieldCount = _metadataEditor.editedFieldCount(),
      .clearCount = _metadataEditor.clearedFieldCount(),
      .tagAddCount = _tagEditor.additionCount(),
      .tagRemoveCount = _tagEditor.removalCount(),
    };
  }

  rt::TrackPropertiesPatch TrackPropertiesEditor::buildPatch() const
  {
    auto patch = rt::TrackPropertiesPatch{.metadata = _metadataEditor.buildPatch()};
    _tagEditor.appendChanges(patch.tagsToAdd, patch.tagsToRemove);
    return patch;
  }

  bool TrackPropertiesEditor::tryHandleEvent(ftxui::Event const& event)
  {
    // A write in flight cannot be steered, cancelled, or escaped from, so the
    // surface stays visible and inert until its terminal result arrives.
    if (_status == TrackEditorStatus::Submitting)
    {
      return true;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      auto const& mouse = mouseEvent.mouse();

      if ((!isLeftPress(mouse) && mouseWheelDirection(mouse) == 0) || !std::exchange(_mouseReady, false))
      {
        return true;
      }

      handleMouse(mouse);
      return true;
    }

    _mouseReady = false;

    if (tryHandleConfirmationEvent(event))
    {
      return true;
    }

    if (_status == TrackEditorStatus::Ready)
    {
      _diagnostic.clear();
    }

    if (_tab == TrackEditorTab::Metadata && _metadataEditor.tryHandleCompletionEvent(event))
    {
      return true;
    }

    // A query filters the tag list and is never part of the draft, so Escape
    // drops it before it says anything about the editor itself.
    if (_tab == TrackEditorTab::Tags && _tagEditor.tryDismissQuery(event))
    {
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      // A draft nobody confirmed losing is worth one question; a clean editor
      // closes at once because there is nothing to lose.
      if (isDirty())
      {
        _confirmingDiscard = true;
      }
      else
      {
        _request = TrackEditorRequest::Close;
      }

      return true;
    }

    if (event == ftxui::Event::CtrlS)
    {
      if (canApply())
      {
        _request = TrackEditorRequest::Apply;
      }

      return true;
    }

    if (event == ftxui::Event::CtrlR)
    {
      // Reload re-reads every captured target, so a draft is genuinely lost.
      if (isDirty())
      {
        _confirmingReload = true;
      }
      else
      {
        _request = TrackEditorRequest::Reload;
      }

      return true;
    }

    if (event == ftxui::Event::Tab)
    {
      cycleTab(1);
      return true;
    }

    if (event == ftxui::Event::TabReverse)
    {
      cycleTab(-1);
      return true;
    }

    switch (_tab)
    {
      case TrackEditorTab::Metadata: _metadataEditor.handleEvent(event); break;
      case TrackEditorTab::Tags: _tagEditor.handleEvent(event); break;
      case TrackEditorTab::Properties: _metadataEditor.handlePropertiesEvent(event); break;
      case TrackEditorTab::Tracks: handleTargetEvent(event); break;
    }

    return true;
  }

  ftxui::Element TrackPropertiesEditor::render() const
  {
    using namespace ftxui;

    auto const title = _targets.size() == 1
                         ? std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorTitle)}
                         : countedText(_textCatalog, MessageId::TuiEditorTitleMultiple, _targets.size());

    _mouseBindings.clear();
    _renderedTab = _tab;
    _mouseReady = true;
    auto header = Elements{text(title) | bold};

    if (isDirty())
    {
      header.push_back(text(" "));
      header.push_back(renderSaveSummary());
    }
    else if (_targets.size() > 1)
    {
      header.push_back(text(" "));
      header.push_back(text(countedText(_textCatalog, MessageId::TuiEditorAppliesToAll, _targets.size())) |
                       style::muted());
    }

    header.push_back(filler());
    header.push_back(_mouseBindings.bind(text(" × "), Event::Escape));

    auto bodyPtr = [&]
    {
      switch (_tab)
      {
        case TrackEditorTab::Tags: return _tagEditor.render();
        case TrackEditorTab::Properties: return _metadataEditor.renderProperties();
        case TrackEditorTab::Tracks: return renderTargetBody();
        case TrackEditorTab::Metadata: break;
      }

      return _metadataEditor.render();
    }();

    return vbox({
             hbox(std::move(header)),
             renderTabStrip(),
             separator(),
             std::move(bodyPtr) | flex | ftxui::reflect(_bodyBox),
             separator(),
             renderFooter(),
           }) |
           border;
  }

  ftxui::Element TrackPropertiesEditor::renderModal(std::int32_t const terminalColumns,
                                                    std::int32_t const terminalRows) const
  {
    using namespace ftxui;

    // A terminal narrower or shorter than the preferred box gets the whole of
    // itself instead, which is what makes 80x24 a full-surface presentation.
    auto const modalCols = std::min(terminalColumns, std::clamp(terminalColumns - 4, 80, 100));
    auto const modalRows = std::min(terminalRows, std::clamp(terminalRows - 2, 24, 30));

    auto boxPtr = render() | size(WIDTH, EQUAL, modalCols) | size(HEIGHT, EQUAL, modalRows) | clear_under;

    if (modalCols >= terminalColumns && modalRows >= terminalRows)
    {
      return boxPtr;
    }

    return vbox({
      filler(),
      hbox({
        filler(),
        std::move(boxPtr),
        filler(),
      }),
      filler(),
    });
  }

  void TrackPropertiesEditor::handleMouse(ftxui::Mouse const& mouse)
  {
    if (_renderedTab != _tab)
    {
      return;
    }

    if (auto const optEvent = _mouseBindings.eventAt(mouse); optEvent)
    {
      _mouseBindings.clear();
      tryHandleEvent(*optEvent);
      return;
    }

    if (_confirmingDiscard || _confirmingReload)
    {
      return;
    }

    if (isLeftPress(mouse))
    {
      if (auto const optTab = mouseRowAt(_tabBoxes, mouse); optTab)
      {
        selectTab(availableTabs()[*optTab]);
        _mouseBindings.clear();
        return;
      }
    }

    if (!containsMouse(_bodyBox, mouse))
    {
      return;
    }

    if (auto const wheel = mouseWheelDirection(mouse); wheel != 0)
    {
      tryHandleEvent(wheel < 0 ? ftxui::Event::ArrowUp : ftxui::Event::ArrowDown);
      return;
    }

    if (auto event = ftxui::Event::Mouse("", mouse); _tab == TrackEditorTab::Metadata)
    {
      if (!_metadataEditor.tryHandleCompletionEvent(event))
      {
        _metadataEditor.handleEvent(event);
      }
    }
    else if (_tab == TrackEditorTab::Tags)
    {
      _tagEditor.handleEvent(event);
    }
  }

  void TrackPropertiesEditor::handleTargetEvent(ftxui::Event const& event)
  {
    if (auto optDelta =
          listNavigationDelta(event, std::max(1, navigationPageRows(_targetViewport) / kTargetRowLines), true);
        optDelta)
    {
      scrollTargets(*optDelta);
    }
  }

  void TrackPropertiesEditor::scrollTargets(std::int32_t const delta)
  {
    auto const count = static_cast<std::int32_t>(_targets.size());

    if (count == 0)
    {
      return;
    }

    auto const target = moveSelection(static_cast<std::int32_t>(_tracksRow), delta, static_cast<std::size_t>(count));
    _tracksRow = static_cast<std::size_t>(target);
  }

  void TrackPropertiesEditor::selectTab(TrackEditorTab const tab)
  {
    if (tab == _tab)
    {
      return;
    }

    _metadataEditor.closeCompletion();
    _tagEditor.clearQuery();
    _tab = tab;
  }

  void TrackPropertiesEditor::cycleTab(std::int32_t const delta)
  {
    auto const tabs = availableTabs();
    auto const it = std::ranges::find(tabs, _tab);
    auto const current = static_cast<std::int32_t>(it != tabs.end() ? it - tabs.begin() : 0);
    auto const count = static_cast<std::int32_t>(tabs.size());
    auto const next = (((current + delta) % count) + count) % count;
    selectTab(tabs[static_cast<std::size_t>(next)]);
  }

  std::vector<TrackEditorTab> TrackPropertiesEditor::availableTabs() const
  {
    auto tabs = std::vector{TrackEditorTab::Metadata, TrackEditorTab::Tags, TrackEditorTab::Properties};

    if (hasTracksTab())
    {
      tabs.push_back(TrackEditorTab::Tracks);
    }

    return tabs;
  }

  bool TrackPropertiesEditor::tryHandleConfirmationEvent(ftxui::Event const& event)
  {
    // A confirmation owns the keyboard until it is answered, so a stray key
    // cannot both dismiss the question and act on the form behind it.
    if (!_confirmingDiscard && !_confirmingReload)
    {
      return false;
    }

    if (event == ftxui::Event::Return)
    {
      _request = _confirmingReload ? TrackEditorRequest::Reload : TrackEditorRequest::Close;
    }
    else if (event != ftxui::Event::Escape)
    {
      return true;
    }

    _confirmingDiscard = false;
    _confirmingReload = false;
    return true;
  }

  ftxui::Element TrackPropertiesEditor::renderTabStrip() const
  {
    using namespace ftxui;

    auto parts = Elements{};

    _tabBoxes.assign(availableTabs().size(), kEmptyMouseBox);

    auto appendTab = [&](TrackEditorTab const tab, MessageId const id)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator("│"));
      }

      auto labelPtr = text(" " + std::string{i18n::requiredText(_textCatalog, id)} + " ");

      if (tab != _tab)
      {
        parts.push_back(std::move(labelPtr) | style::muted() |
                        ftxui::reflect(_tabBoxes[static_cast<std::size_t>(tab)]));
        return;
      }

      // Reverse video pins no palette slot, so which page is open survives a
      // monochrome terminal and any theme.
      parts.push_back(std::move(labelPtr) | inverted | bold | ftxui::reflect(_tabBoxes[static_cast<std::size_t>(tab)]));
    };

    appendTab(TrackEditorTab::Metadata, MessageId::TuiEditorTabMetadata);
    appendTab(TrackEditorTab::Tags, MessageId::TuiEditorTabTags);
    appendTab(TrackEditorTab::Properties, MessageId::TuiEditorTabProperties);

    if (hasTracksTab())
    {
      appendTab(TrackEditorTab::Tracks, MessageId::TuiEditorTabTracks);
    }

    return hbox(std::move(parts));
  }

  ftxui::Element TrackPropertiesEditor::renderTargetBody() const
  {
    using namespace ftxui;

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_targets.size());

    for (auto const& target : _targets)
    {
      rows.push_back(SelectableListRow{
        .elementPtr = vbox({
          text(target.title),
          hbox({text("  "), text(target.path) | style::muted()}),
        }),
      });
    }

    // The focus row is a line coordinate, and each target draws a title line
    // plus a path line, so the target index has to be scaled or the second
    // half of a large capture can never be scrolled into view.
    return selectableList(std::move(rows),
                          SelectableListOptions{.focusRow = static_cast<std::int32_t>(_tracksRow) * kTargetRowLines,
                                                .flex = true,
                                                .viewportBox = &_targetViewport});
  }

  ftxui::Element TrackPropertiesEditor::renderFooter() const
  {
    using namespace ftxui;

    if (_confirmingReload)
    {
      return vbox(
        {paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorReloadPrompt)}) | style::warning(),
         hbox({_mouseBindings.bind(text(" [Enter] "), Event::Return),
               _mouseBindings.bind(text(" [Esc] "), Event::Escape)})});
    }

    if (_confirmingDiscard)
    {
      return vbox(
        {paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorDiscardPrompt)}) | style::warning(),
         hbox({_mouseBindings.bind(text(" [Enter] "), Event::Return),
               _mouseBindings.bind(text(" [Esc] "), Event::Escape)})});
    }

    if (_status == TrackEditorStatus::Submitting)
    {
      return text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorStatusSubmitting)}) | bold;
    }

    if (_status != TrackEditorStatus::Ready)
    {
      auto const message = _status == TrackEditorStatus::Stale
                             ? std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorStatusStale)}
                             : _diagnostic;
      return vbox({
        paragraph(message) | style::danger(),
        hbox({
          _mouseBindings.bind(
            style::shortcutChip("Ctrl-R", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintReload)),
            Event::CtrlR),
          style::mutedSeparator(),
          _mouseBindings.bind(
            style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)), Event::Escape),
        }),
      });
    }

    auto makeRow = [](Elements chips) -> Element
    {
      if (chips.empty())
      {
        return text("");
      }

      auto withSeparators = Elements{};

      for (std::size_t i = 0; i < chips.size(); ++i)
      {
        if (i > 0)
        {
          withSeparators.push_back(style::mutedSeparator());
        }

        withSeparators.push_back(std::move(chips[i]));
      }

      return hbox(std::move(withSeparators));
    };

    auto row1Chips = Elements{};
    auto row2Chips = Elements{};

    if (_tab == TrackEditorTab::Metadata)
    {
      if (_metadataEditor.hasCompletion())
      {
        row1Chips.push_back(
          style::shortcutChip("Up/Down", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintSelect)));
        row1Chips.push_back(_mouseBindings.bind(
          style::shortcutChip("Enter", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintAccept)),
          Event::Return));
        // Escape closes the popup here and leaves the editor open, so naming
        // it "close" would promise the wrong exit.
        row2Chips.push_back(_mouseBindings.bind(
          style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintDismiss)),
          Event::Escape));
      }
      else
      {
        row1Chips.push_back(_mouseBindings.bind(
          style::shortcutChip("Ctrl-D", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClear)),
          Event::CtrlD));
        row1Chips.push_back(_mouseBindings.bind(
          style::shortcutChip("Ctrl-G", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintRestore)),
          Event::CtrlG));

        if (_metadataEditor.supportsCompletion())
        {
          row1Chips.push_back(_mouseBindings.bind(
            style::shortcutChip("Ctrl-N", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintComplete)),
            Event::CtrlN));
        }

        row2Chips.push_back(_mouseBindings.bind(
          style::shortcutChip("Tab", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintPage)), Event::Tab));
        row2Chips.push_back(_mouseBindings.bind(
          style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)), Event::Escape));
      }
    }
    else if (_tab == TrackEditorTab::Tags)
    {
      row1Chips.push_back(
        style::shortcutChip("Up/Down", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintSelect)));
      // The trailing row creates a tag rather than cycling one, so naming the
      // key "toggle" there would describe something the row cannot do.
      row1Chips.push_back(_mouseBindings.bind(
        style::shortcutChip(
          "Enter",
          i18n::requiredText(
            _textCatalog, _tagEditor.isCreatingTag() ? MessageId::TuiEditorHintAdd : MessageId::TuiEditorHintToggle)),
        Event::Return));
      row1Chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Ctrl-G", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintRestore)),
        Event::CtrlG));
      row2Chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Tab", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintPage)), Event::Tab));
      row2Chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)), Event::Escape));
    }
    else
    {
      row2Chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Tab", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintPage)), Event::Tab));
      row2Chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)), Event::Escape));
    }

    auto applyPtr = _mouseBindings.bind(
      style::shortcutChip("Ctrl-S", countedText(_textCatalog, MessageId::TuiEditorApplyAction, _targets.size())),
      Event::CtrlS);
    row2Chips.push_back(canApply() ? std::move(applyPtr) : std::move(applyPtr) | dim);

    if (!_diagnostic.empty())
    {
      return vbox({
        paragraph(_diagnostic) | style::warning(),
        makeRow(std::move(row2Chips)),
      });
    }

    if (row1Chips.empty())
    {
      return makeRow(std::move(row2Chips));
    }

    return vbox({
      makeRow(std::move(row1Chips)),
      makeRow(std::move(row2Chips)),
    });
  }

  ftxui::Element TrackPropertiesEditor::renderSaveSummary() const
  {
    using namespace ftxui;

    auto const summary = patchSummary();
    auto parts = Elements{};

    if (summary.fieldCount > 0)
    {
      parts.push_back(text(countedText(_textCatalog, MessageId::TuiEditorSummaryFields, summary.fieldCount)));
    }

    if (summary.clearCount > 0)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator());
      }

      parts.push_back(text(countedText(_textCatalog, MessageId::TuiEditorSummaryClears, summary.clearCount)) |
                      style::warning());
    }

    if (summary.tagAddCount > 0)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator());
      }

      parts.push_back(
        text(i18n::requiredFormat(_textCatalog, MessageId::TuiEditorSummaryTagsAdd, {{"count", summary.tagAddCount}})));
    }

    if (summary.tagRemoveCount > 0)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator());
      }

      parts.push_back(text(i18n::requiredFormat(
        _textCatalog, MessageId::TuiEditorSummaryTagsRemove, {{"count", summary.tagRemoveCount}})));
    }

    if (_targets.size() > 1 && !parts.empty())
    {
      parts.push_back(style::mutedSeparator());
      parts.push_back(text(countedText(_textCatalog, MessageId::TuiEditorSummaryTargets, _targets.size())) |
                      style::muted());
    }

    return hbox(std::move(parts));
  }
} // namespace ao::tui
