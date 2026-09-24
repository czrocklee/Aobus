// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui::layout::test
{
  // These guards pin the native source wiring, not execution or control flow.
  // They do not prove native dispatch, weak-target liveness, or retirement.
  namespace
  {
    std::string readSource(std::filesystem::path const& path)
    {
      auto stream = std::ifstream{path, std::ios::binary};
      REQUIRE(stream.is_open());
      return std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    // Ignore commented-out wiring and formatting, not arbitrary C++ syntax;
    // comment delimiters inside string literals are outside this lexical guard.
    std::string compactSource(std::string const& source)
    {
      auto const uncommented = std::regex_replace(source, std::regex{R"(/\*[\s\S]*?\*/|//[^\r\n]*)"}, "");
      return std::regex_replace(uncommented, std::regex{R"(\s+)"}, "");
    }

    std::string_view between(std::string_view const source, std::string_view const first, std::string_view const last)
    {
      auto const begin = source.find(first);
      REQUIRE(begin != std::string_view::npos);
      auto const end = source.find(last, begin + first.size());
      REQUIRE(end != std::string_view::npos);
      return source.substr(begin + first.size(), end - begin - first.size());
    }
  } // namespace

  TEST_CASE("ShellCommandWiring - builder source installs owning commands and anchored selector",
            "[winui][unit][layout]")
  {
    auto const builder = readSource(std::filesystem::path{AOBUS_WINDOWS_WINUI_DIR} / "layout/ShellBuilder.cpp");
    auto const constructor =
      compactSource(std::string{between(builder, "ShellBuilder::ShellBuilder(", "ShellBuilder::~ShellBuilder(")});
    auto const registration = compactSource(
      std::string{between(builder, "ShellBuilder::registerActions()", "ShellBuilder::registerComponents()")});

    CHECK(constructor.contains("registerActions();"));
    // The exact call and sink matter: registering an ID without retaining and
    // invoking the offered callback would not wire a document action.
    CHECK(registration.contains("registerShellCommandActions(_config.commands,"
                                "[this](std::string_viewconstid,std::function<void()>command)"
                                "{_actions.registerAction(id,[command=std::move(command)]"
                                "(ActionContextconst&){command();});});"));
    CHECK(registration.contains("if(autoconst&showSelector=_config.showOutputDeviceSelector;showSelector)"
                                "{_actions.registerAction(\"playback.showOutputDeviceSelector\","
                                "[showSelector](ActionContextconst&context){showSelector(context.anchor);});}"));
  }

  TEST_CASE("ShellCommandWiring - window source supplies frame commands and weak anchored selector",
            "[winui][unit][layout]")
  {
    auto const window = readSource(std::filesystem::path{AOBUS_WINDOWS_WINUI_DIR} / "MainWindow.xaml.cpp");
    auto const create =
      compactSource(std::string{between(window, "MainWindow::createShellBuilder()", "MainWindow::shutdown()")});
    auto const config = between(create,
                                "_shellBuilderPtr=std::make_unique<ao::winui::layout::ShellBuilder>("
                                "*_session,ao::winui::layout::ShellBuilderConfig{",
                                ".showOutputDeviceSelector=");
    auto const commandsBegin = config.find(".commands={");
    REQUIRE(commandsBegin != std::string_view::npos);
    auto const commands = config.substr(commandsBegin + std::string_view{".commands={"}.size());
    auto const selector = between(create, ".showOutputDeviceSelector=", ".listCommands=");

    CHECK(selector == "[weak](Microsoft::UI::Xaml::FrameworkElementconst&anchor)"
                      "{if(autoself=weak.get();self){self->showOutputDeviceSelector(anchor);}},");
    CHECK(create.contains("autoweak=get_weak();"));
    CHECK(create.contains("autoconstcommand=[weak](void(MainWindow::*method)())"
                          "{return[weak,method]{if(autoself=weak.get();self)"
                          "{((*self).*method)();}};};"));
    CHECK(commands.contains(".openLibrary=[weak]{if(autoself=weak.get();self)"
                            "{self->pickLibrary();}}"));

    for (auto const& [field, method] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"rescanLibrary", "rescanLibrary"},
           {"toggleInspector", "toggleInspector"},
           {"revealCurrentTrack", "revealCurrentTrack"},
           {"presentTrackProperties", "presentTrackProperties"},
           {"showSoul", "showFullscreenSoul"},
           {"showSystemMenu", "showSystemMenu"},
         }))
    {
      INFO("command " << field);
      CHECK(commands.contains(std::string{"."} + std::string{field} + "=command(&MainWindow::" + std::string{method} +
                              ")"));
    }
  }
} // namespace ao::winui::layout::test
