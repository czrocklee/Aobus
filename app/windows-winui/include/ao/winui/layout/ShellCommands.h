// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>
#include <string_view>

namespace ao::winui::layout
{
  /**
   * @brief WinRT-free, nullary window-frame capabilities for document actions and menus.
   *
   * Callbacks requiring native arguments, such as the output-device selector's
   * anchor, belong to ShellBuilderConfig rather than this capability bundle.
   *
   * A preset names commands; only the frame can run them, because they open
   * pickers, raise system menus, or change what the frame itself presents. They
   * are supplied once and outlive every generation, which is what lets a
   * generation be replaced without re-registering behavior.
   */
  struct ShellCommands final
  {
    std::function<void()> openLibrary{};
    std::function<void()> rescanLibrary{};
    std::function<void()> importLibrary{};
    std::function<void()> exportLibrary{};
    std::function<void()> toggleInspector{};
    std::function<void()> toggleShellMode{};
    std::function<void()> chooseColumns{};
    std::function<void()> reloadTheme{};
    std::function<void()> playPause{};
    std::function<void()> stop{};
    std::function<void()> revealCurrentTrack{};
    std::function<void()> presentTrackProperties{};
    std::function<void()> showSoul{};
    std::function<void()> showSystemMenu{};
  };

  /**
   * @brief Offer the frame's nonempty document-action commands to a registration sink.
   *
   * Calls the sink synchronously with an action id and an owning command copy;
   * the sink is not retained. Menu-only capabilities have no registration here.
   */
  void registerShellCommandActions(ShellCommands const& commands,
                                   std::function<void(std::string_view, std::function<void()>)> const& registerAction);
} // namespace ao::winui::layout
