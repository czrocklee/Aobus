// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Run.h"

#include "CliContext.h"
#include "CommandError.h"
#include "InitCommand.h"
#include "LibCommand.h"
#include "ListCommand.h"
#include "Output.h"
#include "ScanCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"
#include <ao/AppVersion.h>
#include <ao/Exception.h>

#include <CLI/CLI.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <ostream>
#include <print>
#include <string>
#include <vector>

namespace ao::cli
{
  std::int32_t run(std::int32_t argc, char const* const* argv, std::ostream& out, std::ostream& err)
  {
    auto context = CliContext{out, err};
    auto app = CLI::App{"Aobus CLI - aobus"};
    app.require_subcommand(1);
    app.fallthrough();
    app.add_option("-C,--root", context.options().root, "music root")->envname("AOBUS_ROOT");
    auto const outputMapping = std::map<std::string, OutputFormat>{
      {"plain", OutputFormat::Plain}, {"yaml", OutputFormat::Yaml}, {"json", OutputFormat::Json}};
    app.add_option("-O,--output", context.options().format, "output format (plain, yaml, json)")
      ->transform(CLI::CheckedTransformer{outputMapping, CLI::ignore_case});
    app.set_version_flag("--version", kAppVersion);

    try
    {
      setupTrackCommand(app, context);
      setupListCommand(app, context);
      setupInitCommand(app, context);
      setupScanCommand(app, context);
      setupTagCommand(app, context);
      setupLibCommand(app, context);

      app.parse(argc, argv);
      return 0;
    }
    catch (CLI::ParseError const& e)
    {
      return app.exit(e, out, err);
    }
    catch (CommandError const& e)
    {
      std::println(err, "{}", e.what());
      return 1;
    }
    catch (Exception const& e)
    {
      std::println(err, "Internal error: {}\n(at {}:{})\nPlease report this bug.", e.what(), e.file(), e.line());
      return 1;
    }
    catch (std::exception const& e)
    {
      std::println(err, "Error: {}", e.what());
      return 1;
    }
    catch (...)
    {
      std::println(err, "Unknown error occurred");
      return 1;
    }
  }

  std::int32_t run(std::vector<std::string> const& args, std::ostream& out, std::ostream& err)
  {
    auto argv = std::vector<char const*>{};
    argv.reserve(args.size());

    for (auto const& arg : args)
    {
      argv.push_back(arg.c_str());
    }

    return run(static_cast<std::int32_t>(argv.size()), argv.data(), out, err);
  }
} // namespace ao::cli
