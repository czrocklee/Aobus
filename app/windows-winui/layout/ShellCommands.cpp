// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellCommands.h>

#include <ao/uimodel/input/KeymapModel.h>

#include <functional>
#include <string_view>

namespace ao::winui::layout
{
  void registerShellCommandActions(ShellCommands const& commands,
                                   std::function<void(std::string_view, std::function<void()>)> const& registerAction)
  {
    auto const bindCommand = [&registerAction](std::string_view const id, std::function<void()> const& command)
    {
      if (command)
      {
        registerAction(id, command);
      }
    };

    bindCommand("library.open", commands.openLibrary);
    bindCommand("library.rescan", commands.rescanLibrary);
    bindCommand("shell.toggleInspector", commands.toggleInspector);
    bindCommand("shell.showSoul", commands.showSoul);
    bindCommand("shell.showSystemMenu", commands.showSystemMenu);
    bindCommand(uimodel::kRevealCurrentTrackActionId, commands.revealCurrentTrack);
    bindCommand("track.presentProperties", commands.presentTrackProperties);
  }
} // namespace ao::winui::layout
