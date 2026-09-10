// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "Command.h"
#include "MouseBindings.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <list>
#include <span>

namespace ao::tui
{
  struct GoToMenuState final
  {
    rt::NowPlayingInfo nowPlaying{};
    bool canGoBack = false;
    bool canGoForward = false;

    bool operator==(GoToMenuState const&) const = default;
  };

  struct GoToMenuRowHitRegion final
  {
    CommandAction action = CommandAction::RevealCurrentTrack;
    ftxui::Box box = kEmptyMouseBox;
  };

  struct GoToMenuHitRegions final
  {
    GoToMenuState state{};
    std::list<GoToMenuRowHitRegion> rows{};
    ftxui::Box cancelBox = kEmptyMouseBox;
  };

  ftxui::Element goToHintBar(i18n::MessageCatalog const& textCatalog,
                             GoToMenuState const& state,
                             std::int32_t columns,
                             GoToMenuHitRegions* hitRegions = nullptr);

  std::span<CommandAliasSpec const> goToCommands();
  bool canActivateGoTo(GoToMenuState const& state, CommandAction action);
  ftxui::Element goToMenu(i18n::MessageCatalog const& textCatalog,
                          GoToMenuState const& state,
                          std::int32_t selected,
                          std::int32_t columns,
                          GoToMenuHitRegions* hitRegions = nullptr);
} // namespace ao::tui
