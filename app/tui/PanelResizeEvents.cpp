// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EventController.h"
#include "HitRegions.h"
#include "Keymap.h"
#include "LibraryController.h"
#include "ListNavigationModel.h"
#include "MouseBindings.h"
#include "NavigationPanel.h"
#include "PanelResize.h"
#include "PanelWidths.h"
#include "Preferences.h"
#include "ShellInteractionModel.h"

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <optional>
#include <variant>

namespace ao::tui
{
  PanelWidths EventController::panelWidths() const noexcept
  {
    auto const* drag = std::get_if<PanelResizeInteraction>(&_workspaceGesture);
    return drag == nullptr ? _shell.panelWidths() : drag->preview;
  }

  bool EventController::isPanelResizing(HoveredButton const panel) const noexcept
  {
    auto const* drag = std::get_if<PanelResizeInteraction>(&_workspaceGesture);
    return drag != nullptr &&
           panel == (drag->resize.divider() == PanelDivider::Detail ? HoveredButton::DetailToggle
                                                                    : HoveredButton::NavigationToggle);
  }

  std::optional<PanelDivider> EventController::keyboardResizeDivider() const noexcept
  {
    auto const* interaction = std::get_if<PanelResizeInteraction>(&_workspaceGesture);

    if (interaction == nullptr || interaction->optPointerStartX)
    {
      return std::nullopt;
    }

    return interaction->resize.divider();
  }

  NavigationGeometry EventController::panelGeometry() const
  {
    auto const& painted = _hitRegions.navigationLayout;
    return navigationGeometry(painted.terminalColumns,
                              painted.detailColumns,
                              _shell.isNavigationPinned(),
                              _preferences.panelSeparator == "double",
                              panelWidths());
  }

  void EventController::beginKeyboardPanelResize()
  {
    if (_shell.isInputActive() || _shell.overlay() != Overlay::None ||
        (_shell.isNavigationFocused() && _library.navigation().search().isActive()))
    {
      return;
    }

    cancelTransientInteractions();
    auto const geometry = panelGeometry();
    auto const hasDetail = _shell.isDetailVisible() && geometry.detailColumns > 0;

    if ((_shell.isDetailFocused() && !hasDetail) || (!geometry.docked && !hasDetail))
    {
      return;
    }

    auto const divider =
      hasDetail && (_shell.isDetailFocused() || !geometry.docked) ? PanelDivider::Detail : PanelDivider::Navigation;
    _workspaceGesture = PanelResizeInteraction{.resize = PanelResize{_shell.panelWidths(), geometry, divider},
                                               .terminalColumns = geometry.terminalColumns,
                                               .navigationPinned = _shell.isNavigationPinned(),
                                               .detailVisible = _shell.isDetailVisible(),
                                               .preview = _shell.panelWidths()};
  }

  void EventController::finishPanelResize(bool const apply)
  {
    auto const widths = panelWidths();
    cancelWorkspaceGestures();
    _hoveredButton = HoveredButton::None;

    if (apply)
    {
      commitPanelWidths(widths);
    }
  }

  bool EventController::tryHandleKeyboardPanelResize(ftxui::Event const& event)
  {
    if (!keyboardResizeDivider())
    {
      return false;
    }

    if (event == ftxui::Event::Escape)
    {
      finishPanelResize(false);
      return true;
    }

    if (event == ftxui::Event::Return || _keymapPlan.actionFor(event) == KeyAction::BeginPanelResize)
    {
      finishPanelResize(true);
      return true;
    }

    if (event.is_mouse())
    {
      if (auto mouseEvent = event; isLeftPress(mouseEvent.mouse()))
      {
        finishPanelResize(false);
      }

      return true;
    }

    auto& interaction = std::get<PanelResizeInteraction>(_workspaceGesture);
    auto const geometry = panelGeometry();
    auto divider = interaction.resize.divider();

    if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)
    {
      if (geometry.docked && _shell.isDetailVisible() && geometry.detailColumns > 0)
      {
        divider = divider == PanelDivider::Navigation ? PanelDivider::Detail : PanelDivider::Navigation;
        interaction.resize = PanelResize{interaction.preview, geometry, divider};
      }

      return true;
    }

    auto const left = event == ftxui::Event::ArrowLeft || event == ftxui::Event::Character('h') ||
                      event == ftxui::Event::Special("\x1b[1;2D");
    auto const right = event == ftxui::Event::ArrowRight || event == ftxui::Event::Character('l') ||
                       event == ftxui::Event::Special("\x1b[1;2C");

    if (left || right)
    {
      interaction.resize = PanelResize{interaction.preview, geometry, divider};
      interaction.preview = interaction.resize.moved(right ? 1 : -1);
    }

    return true;
  }

  void EventController::commitPanelWidths(PanelWidths const widths)
  {
    if (widths == _shell.panelWidths())
    {
      return;
    }

    _shell.setPanelWidths(widths);

    if (_requestLayoutCheckpoint)
    {
      _requestLayoutCheckpoint();
    }
  }

  bool EventController::tryHandlePanelResizeKey(ftxui::Event const& event)
  {
    auto const left = event == ftxui::Event::Special("\x1b[1;2D");
    auto const right = event == ftxui::Event::Special("\x1b[1;2C");

    if ((!left && !right) || _shell.overlay() != Overlay::None || _shell.isInputActive() ||
        (_shell.isNavigationFocused() && _library.navigation().search().isActive()))
    {
      return false;
    }

    auto const& painted = _hitRegions.navigationLayout;
    auto const geometry = panelGeometry();
    auto const detail = _shell.isDetailFocused() && _shell.isDetailVisible();

    if ((detail && painted.detailColumns <= 0) || (!detail && !(_shell.isNavigationFocused() && geometry.docked)))
    {
      return false;
    }

    auto const resize =
      PanelResize{_shell.panelWidths(), geometry, detail ? PanelDivider::Detail : PanelDivider::Navigation};
    commitPanelWidths(resize.moved(right ? 1 : -1));

    return true;
  }

  bool EventController::tryBeginPanelResize(ftxui::Mouse const& mouse)
  {
    if (!isLeftPress(mouse) || _shell.overlay() != Overlay::None || _shell.isInputActive() ||
        containsMouse(_hitRegions.navigationPinBox, mouse) || containsMouse(_hitRegions.detailToggleBox, mouse))
    {
      return false;
    }

    auto const& geometry = _hitRegions.navigationLayout;
    auto const detail =
      _shell.isDetailVisible() && geometry.detailColumns > 0 && containsMouse(_hitRegions.detailDividerBox, mouse);
    auto const navigation = geometry.docked && containsMouse(_hitRegions.navigationDividerBox, mouse);

    if (!detail && !navigation)
    {
      return false;
    }

    cancelWorkspaceGestures();
    _navigationScrollbarDrag = false;
    _workspaceGesture = PanelResizeInteraction{
      .resize = PanelResize{_shell.panelWidths(), geometry, detail ? PanelDivider::Detail : PanelDivider::Navigation},
      .optPointerStartX = mouse.x,
      .terminalColumns = geometry.terminalColumns,
      .navigationPinned = _shell.isNavigationPinned(),
      .detailVisible = _shell.isDetailVisible(),
      .preview = _shell.panelWidths()};
    _hoveredButton = detail ? HoveredButton::DetailToggle : HoveredButton::NavigationToggle;
    _qualityHoverVisible = false;
    return true;
  }

  bool EventController::tryHandlePanelResizeDrag(ftxui::Mouse const& mouse)
  {
    if ((mouse.motion != ftxui::Mouse::Moved && mouse.motion != ftxui::Mouse::Released) ||
        (mouse.motion == ftxui::Mouse::Released && mouse.button != ftxui::Mouse::Left))
    {
      cancelWorkspaceGestures();
      return true;
    }

    auto& drag = std::get<PanelResizeInteraction>(_workspaceGesture);

    if (!drag.optPointerStartX)
    {
      return false;
    }

    drag.preview = drag.resize.moved(mouse.x - *drag.optPointerStartX);

    if (mouse.motion == ftxui::Mouse::Released)
    {
      auto const widths = drag.preview;
      cancelWorkspaceGestures();
      commitPanelWidths(widths);
      tryHandleMouseMove(mouse);
    }

    return true;
  }
} // namespace ao::tui
