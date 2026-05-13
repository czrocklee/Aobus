// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "InitCommand.h"
#include "LibCommand.h"
#include "ListCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"

#include <CLI/CLI.hpp>
#include <ao/library/MusicLibrary.h>

#include <iostream>

using namespace ao;

int main(int argc, char const* argv[])
{
  try
  {
    auto ml = library::MusicLibrary{"."};

    CLI::App app{"Aobus CLI - aobus"};
    app.require_subcommand(1);

    cli::setupTrackCommand(app, ml);
    cli::setupListCommand(app, ml);
    cli::setupInitCommand(app, ml);
    cli::setupTagCommand(app, ml);
    cli::setupLibCommand(app, ml);

    CLI11_PARSE(app, argc, argv);
    return 0;
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
