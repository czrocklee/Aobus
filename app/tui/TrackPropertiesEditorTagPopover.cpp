// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MouseBindings.h"
#include "Style.h"
#include "TextCell.h"
#include "TrackPropertiesEditor.h"
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  bool TrackPropertiesEditor::tryHandleTagPopoverEvent(ftxui::Event const& event)
  {
    using namespace ftxui;

    if (event.is_mouse())
    {
      auto mouseEvent = event;

      if (auto const& mouse = mouseEvent.mouse();
          (isLeftPress(mouse) || mouseWheelDirection(mouse) != 0) && std::exchange(_mouseReady, false))
      {
        handleTagPopoverMouse(mouseEvent.mouse());
      }

      return true;
    }

    _mouseReady = false;

    if (event == Event::Escape)
    {
      _request = TrackEditorRequest::Close;
      return true;
    }

    if (event == Event::CtrlR)
    {
      if (_status == TrackEditorStatus::Stale || _status == TrackEditorStatus::Failed)
      {
        _request = TrackEditorRequest::Reload;
      }

      return true;
    }

    if (_status != TrackEditorStatus::Ready)
    {
      return true;
    }

    _diagnostic.clear();

    if (event == Event::Return || event == Event::CtrlS)
    {
      if (_tagQueryFocused && _tagEditor.hasNewTagOffer())
      {
        _tagEditor.createTagFromQuery();
      }
      else if (_tagEditor.isCreatingTag())
      {
        _tagEditor.activateFocusedTag();
      }

      if (canApply())
      {
        _request = TrackEditorRequest::Apply;
      }
      else if (!isDirty())
      {
        _request = TrackEditorRequest::Close;
      }

      return true;
    }

    if (event == Event::Tab || event == Event::TabReverse)
    {
      _tagQueryFocused = !_tagQueryFocused;
      return true;
    }

    if (event == Event::ArrowUp || event == Event::ArrowDown || event == Event::PageUp || event == Event::PageDown)
    {
      _tagQueryFocused = false;
    }
    else if (event == Event::Character(' ') && !_tagQueryFocused)
    {
      _tagEditor.activateFocusedTag();
      return true;
    }
    else if (event.is_character() || event == Event::Backspace || event == Event::Delete)
    {
      _tagQueryFocused = true;
    }

    _tagEditor.handleEvent(event);
    return true;
  }

  void TrackPropertiesEditor::handleTagPopoverMouse(ftxui::Mouse const& mouse)
  {
    using namespace ftxui;

    if (isLeftPress(mouse) && !containsMouse(_tagPopoverBox, mouse))
    {
      _request = TrackEditorRequest::Close;
      return;
    }

    if (auto const optEvent = _mouseBindings.eventAt(mouse); optEvent)
    {
      if (*optEvent == Event::Character(' '))
      {
        _tagQueryFocused = false;
      }

      tryHandleTagPopoverEvent(*optEvent);
      return;
    }

    if (_status != TrackEditorStatus::Ready || !containsMouse(_bodyBox, mouse))
    {
      return;
    }

    if (auto const wheel = mouseWheelDirection(mouse); wheel != 0)
    {
      tryHandleTagPopoverEvent(wheel < 0 ? Event::ArrowUp : Event::ArrowDown);
      return;
    }

    if (isLeftPress(mouse))
    {
      _tagQueryFocused = _tagEditor.isQueryHit(mouse);
      _tagEditor.handleEvent(Event::Mouse("", mouse));
    }
  }

  ftxui::Element TrackPropertiesEditor::renderTagPopover(std::int32_t const columns,
                                                         std::int32_t const availableRows) const
  {
    using namespace ftxui;
    using i18n::MessageId;
    _mouseBindings.clear();
    _bodyBox = kEmptyMouseBox;
    _mouseReady = true;
    auto rows = Elements{
      style::panelBody(
        text(i18n::requiredFormat(_textCatalog, MessageId::TuiTagPopoverTitle, {{"count", _targets.size()}})) |
        style::accent() | bold | xflex),
    };

    if (_targets.size() == 1 && availableRows >= kTagPopoverNavigationRows)
    {
      rows.push_back(style::panelBody(text(_targets.front().title) | xflex | dim));
    }

    rows.push_back(style::panelBody(separator()));

    if (_status == TrackEditorStatus::Stale || _status == TrackEditorStatus::Failed)
    {
      auto const message = _status == TrackEditorStatus::Stale
                             ? std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorStatusStale)}
                             : _diagnostic;
      rows.push_back(style::panelBody(paragraph(message) | style::danger() | yframe) | flex);
    }
    else
    {
      rows.push_back(style::scrollablePanelBody(_tagEditor.render(_tagQueryFocused, columns - 4)) | reflect(_bodyBox));
    }

    rows.push_back(style::panelBody(separator()));

    if (_status == TrackEditorStatus::Ready && !_diagnostic.empty())
    {
      rows.push_back(style::panelBody(paragraph(_diagnostic) | style::warning()));
    }

    rows.push_back(style::panelBody(renderTagPopoverFooter(columns - 4, availableRows >= kTagPopoverNavigationRows)));
    return vbox(std::move(rows)) | border | reflect(_tagPopoverBox);
  }

  ftxui::Element TrackPropertiesEditor::renderTagPopoverFooter(std::int32_t const columns,
                                                               bool const showNavigation) const
  {
    using namespace ftxui;
    using i18n::MessageId;

    auto hint = [&](std::string_view key, MessageId label, Event event)
    {
      return _mouseBindings.bind(style::shortcutChip(key,
                                                     ellipsizeToCellWidth(i18n::requiredText(_textCatalog, label),
                                                                          std::max(0, columns - cellWidth(key) - 1))),
                                 std::move(event));
    };

    if (_status == TrackEditorStatus::Submitting)
    {
      return text(
               ellipsizeToCellWidth(i18n::requiredText(_textCatalog, MessageId::TuiEditorStatusSubmitting), columns)) |
             bold;
    }

    if (_status != TrackEditorStatus::Ready)
    {
      return vbox({hbox({filler(), hint("Ctrl-R", MessageId::TuiEditorHintReload, Event::CtrlR)}),
                   hbox({filler(), hint("Esc", MessageId::TuiTagPopoverCancel, Event::Escape)})});
    }

    auto rows = Elements{};

    if (showNavigation)
    {
      auto navigation = Elements{filler(),
                                 _mouseBindings.bind(text("↑ ") | style::accent() | bold, Event::ArrowUp),
                                 hint("↓", MessageId::TuiEditorHintSelect, Event::ArrowDown)};

      if (!_tagQueryFocused)
      {
        auto const toggleLabel =
          _tagEditor.isCreatingTag() ? MessageId::TuiEditorHintAdd : MessageId::TuiEditorHintToggle;
        auto togglePtr = hint("Space", toggleLabel, Event::Character(' '));
        auto const navigationColumns = 13 +
                                       cellWidth(i18n::requiredText(_textCatalog, MessageId::TuiEditorHintSelect)) +
                                       cellWidth(i18n::requiredText(_textCatalog, toggleLabel));

        if (navigationColumns > columns)
        {
          rows.push_back(hbox(std::move(navigation)));
          navigation = Elements{filler(), std::move(togglePtr)};
        }
        else
        {
          navigation.push_back(style::mutedSeparator());
          navigation.push_back(std::move(togglePtr));
        }
      }

      rows.push_back(hbox(std::move(navigation)));
    }

    auto const createsTag = _tagQueryFocused ? _tagEditor.hasNewTagOffer() : _tagEditor.isCreatingTag();
    auto const applyLabel = createsTag ? MessageId::TuiTagPopoverAddApply : MessageId::TuiTagPopoverApply;
    auto applyPtr = hint("Enter", applyLabel, Event::Return);
    auto cancelPtr = hint("Esc", MessageId::TuiTagPopoverCancel, Event::Escape);
    auto const actionColumns = 13 + cellWidth(i18n::requiredText(_textCatalog, applyLabel)) +
                               cellWidth(i18n::requiredText(_textCatalog, MessageId::TuiTagPopoverCancel));

    if (actionColumns > columns)
    {
      rows.push_back(hbox({filler(), std::move(applyPtr)}));
      rows.push_back(hbox({filler(), std::move(cancelPtr)}));
    }
    else
    {
      rows.push_back(hbox({filler(), std::move(applyPtr), style::mutedSeparator(), std::move(cancelPtr)}));
    }

    return vbox(std::move(rows));
  }
} // namespace ao::tui
