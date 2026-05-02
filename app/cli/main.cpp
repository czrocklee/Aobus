// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <CLI/CLI.hpp>
#include <ao/library/MusicLibrary.h>

#include "InitCommand.h"
#include "LibCommand.h"
#include "ListCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"

#include <iostream>

int main(int argc, char const* argv[])
{
  try
  {
    auto ml = ao::library::MusicLibrary{"."};

    CLI::App app{"RockStudio CLI - rsc"};
    app.require_subcommand(1);

    ao::tool::setupTrackCommand(app, ml);
    ao::tool::setupListCommand(app, ml);
    ao::tool::setupInitCommand(app, ml);
    ao::tool::setupTagCommand(app, ml);
    ao::tool::setupLibCommand(app, ml);

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
