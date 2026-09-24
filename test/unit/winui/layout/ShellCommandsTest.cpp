// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellCommands.h>

#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::winui::layout::test
{
  namespace
  {
    using RegisteredActions = std::map<std::string, std::function<void()>, std::less<>>;

    RegisteredActions collectCommandActions(ShellCommands const& commands)
    {
      auto actions = RegisteredActions{};
      registerShellCommandActions(commands,
                                  [&actions](std::string_view const id, std::function<void()> command)
                                  {
                                    auto const inserted = actions.emplace(std::string{id}, std::move(command)).second;
                                    REQUIRE(inserted);
                                  });
      return actions;
    }

    ShellCommands nonemptyCommands()
    {
      return {
        .openLibrary = [] {},
        .rescanLibrary = [] {},
        .toggleInspector = [] {},
        .revealCurrentTrack = [] {},
        .presentTrackProperties = [] {},
        .showSoul = [] {},
        .showSystemMenu = [] {},
      };
    }
  } // namespace

  TEST_CASE("registerShellCommandActions - each registered action invokes its own retained command",
            "[winui][unit][layout]")
  {
    auto invoked = std::vector<std::string_view>{};
    auto actions = RegisteredActions{};
    auto const fields =
      std::to_array<std::tuple<std::string_view, std::string_view, std::function<void()> ShellCommands::*>>({
        {"library.open", "openLibrary", &ShellCommands::openLibrary},
        {"library.rescan", "rescanLibrary", &ShellCommands::rescanLibrary},
        {"shell.toggleInspector", "toggleInspector", &ShellCommands::toggleInspector},
        {"shell.showSoul", "showSoul", &ShellCommands::showSoul},
        {"shell.showSystemMenu", "showSystemMenu", &ShellCommands::showSystemMenu},
        {uimodel::kRevealCurrentTrackActionId, "revealCurrentTrack", &ShellCommands::revealCurrentTrack},
        {"track.presentProperties", "presentTrackProperties", &ShellCommands::presentTrackProperties},
      });
    auto const verifyActions = [&]
    {
      for (auto const& entry : fields)
      {
        auto const& id = std::get<0>(entry);
        auto const& expected = std::get<1>(entry);
        INFO("action " << id);
        auto const found = actions.find(id);
        REQUIRE(found != actions.end());
        REQUIRE(found->second);
        found->second();
        REQUIRE(invoked == std::vector<std::string_view>{expected});
        invoked.clear();
      }
    };

    {
      auto commands = ShellCommands{
        .openLibrary = [&] { invoked.emplace_back("openLibrary"); },
        .rescanLibrary = [&] { invoked.emplace_back("rescanLibrary"); },
        .toggleInspector = [&] { invoked.emplace_back("toggleInspector"); },
        .revealCurrentTrack = [&] { invoked.emplace_back("revealCurrentTrack"); },
        .presentTrackProperties = [&] { invoked.emplace_back("presentTrackProperties"); },
        .showSoul = [&] { invoked.emplace_back("showSoul"); },
        .showSystemMenu = [&] { invoked.emplace_back("showSystemMenu"); },
      };
      actions = collectCommandActions(commands);
      REQUIRE(actions.size() == fields.size());

      for (auto const& entry : fields)
      {
        commands.*std::get<2>(entry) = [&invoked] { invoked.emplace_back("replacement"); };
      }

      // Fail before retirement if a registration borrowed the source bundle;
      // otherwise invoking that mutant after destruction would dangle.
      verifyActions();
    }

    // The input command bundle has retired; the same registrations must still
    // reach the original matching callbacks after their source is destroyed.
    verifyActions();
  }

  TEST_CASE("registerShellCommandActions - empty capabilities are omitted individually and together",
            "[winui][unit][layout]")
  {
    auto const fields = std::to_array<std::pair<std::string_view, std::function<void()> ShellCommands::*>>({
      {"library.open", &ShellCommands::openLibrary},
      {"library.rescan", &ShellCommands::rescanLibrary},
      {"shell.toggleInspector", &ShellCommands::toggleInspector},
      {"shell.showSoul", &ShellCommands::showSoul},
      {"shell.showSystemMenu", &ShellCommands::showSystemMenu},
      {uimodel::kRevealCurrentTrackActionId, &ShellCommands::revealCurrentTrack},
      {"track.presentProperties", &ShellCommands::presentTrackProperties},
    });

    for (auto const& [omittedId, field] : fields)
    {
      INFO("omitted " << omittedId);
      auto commands = nonemptyCommands();
      commands.*field = {};
      auto const actions = collectCommandActions(commands);

      REQUIRE(actions.size() == fields.size() - 1);

      for (auto const& entry : fields)
      {
        CHECK(actions.contains(entry.first) == (entry.first != omittedId));
      }
    }

    auto const actions = collectCommandActions(ShellCommands{});
    CHECK(actions.empty());
  }

  TEST_CASE("registerShellCommandActions - menu-only capabilities are not document registrations",
            "[winui][unit][layout]")
  {
    auto const commands = ShellCommands{
      .importLibrary = [] {},
      .exportLibrary = [] {},
      .toggleShellMode = [] {},
      .chooseColumns = [] {},
      .reloadTheme = [] {},
      .playPause = [] {},
      .stop = [] {},
    };
    auto const actions = collectCommandActions(commands);

    CHECK(actions.empty());
  }
} // namespace ao::winui::layout::test
