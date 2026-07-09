// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.h"
#include <ao/AppVersion.h>
#include <ao/Exception.h>
#include <ao/rt/Log.h>

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <print>
#include <span>
#include <string>

namespace
{
  ao::tui::AppOptions parseOptions(std::span<char*> args)
  {
    auto options = ao::tui::AppOptions{};
    auto app = CLI::App{"Aobus terminal player"};

    app.add_option("-l,--library", options.libraryRoot, "Music library root")->capture_default_str();
    app.add_option("--database", options.databasePath, "Aobus library database path");
    app.add_option("--config", options.configPath, "TUI workspace config path");
    app.add_option("--cover-art-mode", options.coverArtMode, "Cover art renderer: auto, kitty, blocks, off")
      ->check(CLI::IsMember({"auto", "kitty", "blocks", "off"}))
      ->capture_default_str();
    app.add_flag_callback(
      "--version",
      []
      {
        std::println("Aobus TUI {}", ao::kAppVersion);
        std::exit(0);
      },
      "Show version information");

    auto const logMapping = std::map<std::string, ao::rt::LogLevel>{
      {"trace", ao::rt::LogLevel::Trace},
      {"debug", ao::rt::LogLevel::Debug},
      {"info", ao::rt::LogLevel::Info},
      {"warn", ao::rt::LogLevel::Warn},
      {"error", ao::rt::LogLevel::Error},
      {"critical", ao::rt::LogLevel::Critical},
      {"off", ao::rt::LogLevel::Off},
    };

    app.add_option("--log-level", options.logLevel, "Set the logging level")
      ->transform(CLI::CheckedTransformer{logMapping, CLI::ignore_case});

    try
    {
      app.parse(static_cast<std::int32_t>(args.size()), args.data());
    }
    catch (CLI::ParseError const& e)
    {
      std::exit(app.exit(e));
    }

    options.libraryRoot = std::filesystem::absolute(options.libraryRoot).lexically_normal();

    if (options.databasePath.empty())
    {
      options.databasePath = options.libraryRoot / ".aobus" / "library";
    }

    if (options.configPath.empty())
    {
      options.configPath = options.libraryRoot / ".aobus" / "tui-workspace.yaml";
    }

    options.databasePath = std::filesystem::absolute(options.databasePath).lexically_normal();
    options.configPath = std::filesystem::absolute(options.configPath).lexically_normal();
    return options;
  }
} // namespace

int main(int argc, char* argv[])
{
  try
  {
    return ao::tui::run(parseOptions({argv, static_cast<std::size_t>(argc)}));
  }
  catch (ao::Exception const& e)
  {
    std::println(stderr, "Internal error: {}\n(at {}:{})", e.what(), e.file(), e.line());
    ao::rt::Log::shutdown();
    return 1;
  }
  catch (std::exception const& e)
  {
    std::println(stderr, "Error: {}", e.what());
    ao::rt::Log::shutdown();
    return 1;
  }
}
