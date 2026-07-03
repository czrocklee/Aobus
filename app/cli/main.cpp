// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "InitCommand.h"
#include "LibCommand.h"
#include "ListCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"
#include <ao/Exception.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/rt/CoreRuntime.h>

#include <CLI/CLI.hpp>

#include <exception>
#include <memory>
#include <print>
#include <utility>

using namespace ao;

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char const* argv[])
{
  try
  {
    auto executorPtr = std::make_unique<async::ImmediateExecutor>();
    auto runtime = rt::CoreRuntime{std::move(executorPtr), ".", ".aobus/library"};

    auto app = CLI::App{"Aobus CLI - aobus"};
    app.require_subcommand(1);

    cli::setupTrackCommand(app, runtime);
    cli::setupListCommand(app, runtime);
    cli::setupInitCommand(app, runtime);
    cli::setupTagCommand(app, runtime);
    cli::setupLibCommand(app, runtime);

    CLI11_PARSE(app, argc, argv);
    return 0;
  }
  catch (ao::Exception const& e)
  {
    std::println(stderr, "Internal error: {}\n(at {}:{})\nPlease report this bug.", e.what(), e.file(), e.line());
    return 1;
  }
  catch (std::exception const& e)
  {
    std::println(stderr, "Error: {}", e.what());
    return 1;
  }
  catch (...)
  {
    std::println(stderr, "Unknown error occurred");
    return 1;
  }
}
