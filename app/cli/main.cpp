// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "InitCommand.h"
#include "LibCommand.h"
#include "ListCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"
#include <ao/Exception.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ImmediateControlExecutor.h>

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <utility>

using namespace ao;

int main(int argc, char const* argv[])
{
  try
  {
    auto executorPtr = std::make_unique<rt::ImmediateControlExecutor>();
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
    std::cerr << "Internal error: " << e.what() << "\n(at " << e.file() << ":" << e.line() << ")\n"
              << "Please report this bug.\n";
    return 1;
  }
  catch (std::exception const& e)
  {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cerr << "Unknown error occurred" << '\n';
    return 1;
  }
}
