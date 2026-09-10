// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EventController.h"
#include "HitRegions.h"
#include "Keymap.h"
#include "ListNavigationModel.h"
#include "MouseBindings.h"
#include "SelectionNavigation.h"
#include "ShellInteractionModel.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace ao::tui
{
  void EventController::reportNavigationPinChange(bool const previous)
  {
    if (previous != _shell.isNavigationPinned() && _requestLayoutCheckpoint)
    {
      _requestLayoutCheckpoint();
    }
  }

  void EventController::leaveNavigation()
  {
    if (_shell.isNavigationFocused())
    {
      cancelTransientInteractions();
      _navigationScrollbarDrag = false;
      _library.navigation().clearSearch();
      _shell.focusTracks();
    }
  }

  void EventController::toggleLists()
  {
    cancelTransientInteractions();
    _navigationScrollbarDrag = false;
    _shell.toggleNavigation(_hitRegions.navigationLayout.canDock);

    if (_shell.overlay() == Overlay::ListChooser)
    {
      _library.revealActiveList();
    }

    _library.navigation().clearSearch();

    if (_shell.isNavigationFocused())
    {
      _library.navigation().reveal(_library.currentListId());
    }
  }

  void EventController::togglePinnedLists()
  {
    cancelTransientInteractions();

    if (_shell.overlay() == Overlay::ListChooser)
    {
      closeOverlay();
    }

    auto const pinned = _shell.isNavigationPinned();
    _shell.toggleNavigationPin();
    _hitRegions.navigationDividerBox = kEmptyMouseBox;
    _library.navigation().clearSearch();

    if (_shell.isNavigationFocused())
    {
      _library.navigation().reveal(_library.currentListId());
    }

    _shell.reconcileNavigationLayout(panelGeometry().canDock);
    reportNavigationPinChange(pinned);
  }

  void EventController::switchWorkspaceFocus()
  {
    if (_shell.isNavigationFocused())
    {
      leaveNavigation();
      return;
    }

    _shell.switchWorkspaceFocus(_hitRegions.navigationLayout.canDock);

    if (_shell.isNavigationFocused())
    {
      cancelTransientInteractions();
      _library.navigation().reveal(_library.currentListId());
    }
  }

  void EventController::activateNavigation(bool const keyboard)
  {
    auto const optTarget = _library.navigation().activationTarget();

    if (!optTarget)
    {
      return;
    }

    if (auto const res = _library.openList(*optTarget); !res)
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        i18n::requiredFormat(
          _library.textCatalog(), i18n::MessageId::TuiNavigationOpenFailed, {{"detail", res.error().message}}));
      return;
    }

    cancelTransientInteractions();

    if (keyboard || !_hitRegions.navigationLayout.docked)
    {
      leaveNavigation();
    }
  }

  bool EventController::tryHandleNavigationEvent(ftxui::Event const& event)
  {
    if (!_shell.isNavigationFocused() || _shell.isInputActive() || isModalOverlay(_shell.overlay()))
    {
      return false;
    }

    auto& model = _library.navigation();
    auto const searching = model.search().isActive();

    if (searching && (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse))
    {
      leaveNavigation();
      return true;
    }

    if (model.trySearchEvent(event))
    {
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      leaveNavigation();
      return true;
    }

    if (auto const optDelta =
          listNavigationDelta(event, navigationPageRows(_hitRegions.navigation.panel.navigationBox), !searching);
        optDelta)
    {
      if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
      {
        model.selectVisibleRow(model.selectedIndex() + *optDelta);
      }
      else
      {
        model.move(*optDelta);
      }

      return true;
    }

    if (event == ftxui::Event::Return)
    {
      activateNavigation(true);
      return true;
    }

    if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight)
    {
      if (event == ftxui::Event::ArrowLeft)
      {
        model.collapse();
      }
      else
      {
        model.expand();
      }

      return true;
    }

    if (auto const optAction = _keymapPlan.actionFor(event); optAction)
    {
      if (isPlaybackControl(*optAction))
      {
        executeKeyAction(*optAction);
        return true;
      }

      switch (*optAction)
      {
        case KeyAction::BeginPanelResize:
        case KeyAction::SwitchWorkspaceFocus:
        case KeyAction::TogglePinnedLists:
        case KeyAction::ToggleLists:
        case KeyAction::FocusDetails:
        case KeyAction::ToggleDetails:
        case KeyAction::ToggleAudioPipeline:
        case KeyAction::ToggleOutputDevices:
        case KeyAction::TogglePresentations:
        case KeyAction::ToggleNotifications:
        case KeyAction::OpenCommandPalette:
        case KeyAction::ShowHelp:
        case KeyAction::OpenSettings:
        case KeyAction::OpenGoTo:
        case KeyAction::OpenCurrentArtist:
        case KeyAction::OpenCurrentAlbum:
        case KeyAction::WorkspaceBack:
        case KeyAction::WorkspaceForward:
        case KeyAction::RevealCurrentTrack:
        case KeyAction::Quit: executeKeyAction(*optAction); break;
        default: break;
      }
    }

    return true;
  }

  void EventController::selectNavigationFromScrollbar(std::int32_t const row)
  {
    auto& model = _library.navigation();
    auto const& box = _hitRegions.navigation.panel.navigationBox;

    if (model.rows().empty() || box.IsEmpty())
    {
      return;
    }

    auto const height = std::max(1, box.y_max - box.y_min);
    auto const offset = std::clamp(row - box.y_min, 0, height);
    auto const index = static_cast<std::size_t>(offset) * (model.rows().size() - 1) / static_cast<std::size_t>(height);
    model.selectVisibleRow(static_cast<std::int32_t>(index));
  }

  void EventController::scrollDetail(std::int32_t const delta)
  {
    auto const& regions = _hitRegions.detailPanel;
    auto const lastRow = std::max(0,
                                  regions.contentBox.y_max - regions.contentBox.y_min -
                                    (regions.navigationBox.y_max - regions.navigationBox.y_min));
    auto const offset = _shell.detailSections().revealSelected
                          ? std::max(0, regions.navigationBox.y_min - regions.contentBox.y_min)
                          : std::min(_shell.detailScroll(), lastRow);
    _shell.resetDetailScroll();
    _shell.scrollDetail(offset, lastRow);
    _shell.scrollDetail(delta, lastRow);
  }

  bool EventController::tryHandleDetailEvent(ftxui::Event const& event)
  {
    if (!_shell.isDetailFocused() || _shell.isInputActive() || isModalOverlay(_shell.overlay()))
    {
      return false;
    }

    auto& sections = _shell.detailSections();

    if (event == ftxui::Event::Escape)
    {
      _shell.focusTracks();
      return true;
    }

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k') || event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::Character('j'))
    {
      sections.selected = event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k') ? 0 : 1;
      sections.revealSelected = true;
      return true;
    }

    if (event == ftxui::Event::Return || event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight)
    {
      if (_library.selectedTrackView().track != nullptr)
      {
        auto& expanded = sections.expanded[sections.selected];
        expanded = event == ftxui::Event::Return ? !expanded : event == ftxui::Event::ArrowRight;
        sections.revealSelected = true;
      }

      return true;
    }

    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
    {
      scrollDetail((event == ftxui::Event::PageUp ? -1 : 1) *
                   navigationPageRows(_hitRegions.detailPanel.navigationBox));
      return true;
    }

    if (auto const optAction = _keymapPlan.actionFor(event); optAction)
    {
      switch (*optAction)
      {
        case KeyAction::PlaySelection:
        case KeyAction::PreviousRow:
        case KeyAction::NextRow:
        case KeyAction::PreviousSection:
        case KeyAction::NextSection:
        case KeyAction::SelectToggle:
        case KeyAction::SelectVisual:
        case KeyAction::SelectAll:
        case KeyAction::SelectClear:
        case KeyAction::EditProperties: return true;
        default: executeKeyAction(*optAction); break;
      }
    }

    return true;
  }

  std::optional<bool> EventController::tryHandleDetailMouse(ftxui::Mouse const& mouse)
  {
    if (_shell.isInputActive() || isModalOverlay(_shell.overlay()))
    {
      return std::nullopt;
    }

    if (containsMouse(_hitRegions.detailToggleBox, mouse))
    {
      if (mouse.motion == ftxui::Mouse::Moved)
      {
        return tryHandleMouseMove(mouse);
      }

      if (isLeftPress(mouse))
      {
        toggleDetailPanel();
      }

      return true;
    }

    if (mouse.motion == ftxui::Mouse::Moved && containsMouse(_hitRegions.detailDividerBox, mouse))
    {
      return tryHandleMouseMove(mouse);
    }

    if (!_shell.isDetailVisible() || !containsMouse(_hitRegions.detailPanel.box, mouse))
    {
      return std::nullopt;
    }

    if (auto const wheel = mouseWheelDirection(mouse); wheel != 0)
    {
      scrollDetail(wheel * _preferences.wheelStep);
    }

    if (isLeftPress(mouse) && containsMouse(_hitRegions.detailPanel.navigationBox, mouse))
    {
      if (auto const* track = _library.selectedTrackView().track;
          track != nullptr && track->id == _hitRegions.detailTrack)
      {
        for (std::size_t i = 0; i < _hitRegions.detailSectionBoxes.size(); ++i)
        {
          if (containsMouse(_hitRegions.detailSectionBoxes[i], mouse))
          {
            scrollDetail(0);
            auto& sections = _shell.detailSections();
            sections.expanded[i] = !sections.expanded[i];
            break;
          }
        }
      }
    }

    _hoveredButton = HoveredButton::None;
    _qualityHoverVisible = false;
    return true;
  }

  std::optional<bool> EventController::tryHandleNavigationMouse(ftxui::Mouse const& mouse)
  {
    if (_shell.isInputActive() || isModalOverlay(_shell.overlay()))
    {
      _navigationScrollbarDrag = false;
      return std::nullopt;
    }

    if (_shell.isNavigationFocused() && containsMouse(_hitRegions.navigationSearchBox, mouse))
    {
      tryHandleMouseMove(mouse);

      if (isLeftPress(mouse) && !_library.navigation().search().isActive())
      {
        _library.navigation().trySearchEvent(ftxui::Event::Character("/"));
      }

      return true;
    }

    if (containsMouse(_hitRegions.navigationPinBox, mouse))
    {
      if (mouse.motion == ftxui::Mouse::Moved)
      {
        return tryHandleMouseMove(mouse);
      }

      if (isLeftPress(mouse))
      {
        togglePinnedLists();
      }

      return true;
    }

    auto const& geometry = _hitRegions.navigationLayout;
    auto const& regions = _hitRegions.navigation;

    if (!geometry.docked || regions.panel.box.IsEmpty())
    {
      _navigationScrollbarDrag = false;
      return std::nullopt;
    }

    if (_navigationScrollbarDrag && regions.revision != _library.navigation().revision())
    {
      _navigationScrollbarDrag = false;
      return true;
    }

    if (_navigationScrollbarDrag)
    {
      if (mouse.motion == ftxui::Mouse::Released)
      {
        _navigationScrollbarDrag = false;
      }
      else if (mouse.motion == ftxui::Mouse::Moved)
      {
        selectNavigationFromScrollbar(mouse.y);
      }

      return true;
    }

    if (mouse.motion == ftxui::Mouse::Moved && containsMouse(_hitRegions.navigationDividerBox, mouse))
    {
      return tryHandleMouseMove(mouse);
    }

    if (!containsMouse(regions.panel.box, mouse))
    {
      return std::nullopt;
    }

    _qualityHoverVisible = false;
    _hoveredButton = HoveredButton::None;

    if (auto const direction = mouseWheelDirection(mouse); direction != 0)
    {
      _lastClickedTrack = kInvalidTrackId;
      _library.navigation().move(direction * _preferences.wheelStep);
      return true;
    }

    if (!isLeftPress(mouse))
    {
      return true;
    }

    handleNavigationPress(mouse);
    return true;
  }

  void EventController::handleNavigationPress(ftxui::Mouse const& mouse)
  {
    auto const& regions = _hitRegions.navigation;

    if (regions.revision != _library.navigation().revision())
    {
      return;
    }

    cancelTransientInteractions();
    _shell.focusNavigation();
    auto& model = _library.navigation();

    if (model.trySearchEvent(ftxui::Event::Mouse("", mouse)))
    {
      return;
    }

    if (auto const& viewport = regions.panel.navigationBox; containsMouse(viewport, mouse) && mouse.x == viewport.x_max)
    {
      _navigationScrollbarDrag = true;
      selectNavigationFromScrollbar(mouse.y);
      return;
    }

    for (auto const& hit : regions.rows)
    {
      if (!containsMouse(hit.row, mouse))
      {
        continue;
      }

      auto const current = std::ranges::find(model.rows(), hit.id, &ListNavigationRow::id);

      if (current == model.rows().end())
      {
        return;
      }

      if (containsMouse(hit.disclosure, mouse) && current->hasChildren)
      {
        model.toggleExpanded(hit.id);
      }
      else if (model.trySelect(hit.id))
      {
        activateNavigation(false);
      }

      return;
    }
  }

  void EventController::syncWorkspaceGeometry()
  {
    if (auto const& box = _hitRegions.trackTableBox;
        _lastNavigationGeometry != _hitRegions.navigationLayout || box.x_min != _lastTrackTableBox.x_min ||
        box.x_max != _lastTrackTableBox.x_max || box.y_min != _lastTrackTableBox.y_min ||
        box.y_max != _lastTrackTableBox.y_max)
    {
      auto const* drag = std::get_if<PanelResizeInteraction>(&_workspaceGesture);
      auto const ownReflow = drag != nullptr && drag->terminalColumns == _hitRegions.navigationLayout.terminalColumns &&
                             drag->navigationPinned == _shell.isNavigationPinned() &&
                             drag->detailVisible == _shell.isDetailVisible() && box.y_min == _lastTrackTableBox.y_min &&
                             box.y_max == _lastTrackTableBox.y_max;

      if (!ownReflow)
      {
        cancelWorkspaceGestures();
      }

      _lastClickedTrack = kInvalidTrackId;
      _navigationScrollbarDrag = false;
      _lastNavigationGeometry = _hitRegions.navigationLayout;
      _lastTrackTableBox = box;
    }
  }
} // namespace ao::tui
