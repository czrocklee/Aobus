// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <CLI/CLI.hpp>
#include <rs/core/MusicLibrary.h>

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
    auto ml = rs::core::MusicLibrary{"."};

    CLI::App app{"RockStudio CLI - rsc"};
    app.require_subcommand(1);

    rs::tool::setupTrackCommand(app, ml);
    rs::tool::setupListCommand(app, ml);
    rs::tool::setupInitCommand(app, ml);
    rs::tool::setupTagCommand(app, ml);
    rs::tool::setupLibCommand(app, ml);

    CLI11_PARSE(app, argc, argv);
    return 0;
  }
  catch (std::exception const& e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cerr << "Unknown error occurred" << std::endl;
    return 1;
  }
}
