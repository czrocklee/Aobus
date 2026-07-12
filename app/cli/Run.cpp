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
  namespace
  {
    bool hasHelpAllArgument(std::int32_t argc, char const* const* argv)
    {
      for (std::int32_t i = 1; i < argc; ++i)
      {
        if (std::string_view{argv[i]} == "--help-all")
        {
          return true;
        }
      }

      return false;
    }

    void writeHelpTree(CLI::App const& app, std::string const& commandPath, std::ostream& out)
    {
      out << app.help(commandPath, CLI::AppFormatMode::Normal);

      for (auto const* const subcommand :
           app.get_subcommands([](CLI::App const* sub) { return !sub->get_name().empty(); }))
      {
        out << "\n\n";
        writeHelpTree(*subcommand, commandPath + " " + subcommand->get_name(), out);
      }
    }
  } // namespace

  std::int32_t run(std::int32_t argc,
                   char const* const* argv,
                   std::ostream& out,
                   std::ostream& err,
                   CliRunOptions const options)
  {
    auto context = CliContext{out, err, options.musicLibraryMapSize};
    auto app = CLI::App{"Aobus CLI - aobus"};
    app.require_subcommand(1);
    app.fallthrough();
    app.add_option("-C,--root", context.options().root, "music root")->envname("AOBUS_ROOT");
    auto const outputMapping = std::map<std::string, OutputFormat>{
      {"plain", OutputFormat::Plain}, {"yaml", OutputFormat::Yaml}, {"json", OutputFormat::Json}};
    app.add_option("-O,--output", context.options().format, "output format (plain, yaml, json)")
      ->transform(CLI::CheckedTransformer{outputMapping, CLI::ignore_case});
    app.set_help_all_flag("--help-all", "Print this help message and all subcommand help");
    app.set_version_flag("--version", kAppVersion);

    try
    {
      configureTrackCommand(app, context);
      configureListCommand(app, context);
      configureInitCommand(app, context);
      configureScanCommand(app, context);
      configureTagCommand(app, context);
      configureLibCommand(app, context);

      if (hasHelpAllArgument(argc, argv))
      {
        // CLI11's built-in help-all still runs after subcommand requirements,
        // and its All mode expands only one level. Print the full tree before
        // parse() so `aobus --help-all` works as a complete agent-facing
        // command reference.
        writeHelpTree(app, "aobus", out);
        return 0;
      }

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

  std::int32_t run(std::vector<std::string> const& args,
                   std::ostream& out,
                   std::ostream& err,
                   CliRunOptions const options)
  {
    auto argv = std::vector<char const*>{};
    argv.reserve(args.size());

    for (auto const& argument : args)
    {
      argv.push_back(argument.c_str());
    }

    return run(static_cast<std::int32_t>(argv.size()), argv.data(), out, err, options);
  }
} // namespace ao::cli
