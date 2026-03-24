// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <boost/timer/timer.hpp>

#include <rs/core/MusicLibrary.h>

#include "ComboCommand.h"
#include "InitCommand.h"
#include "ListCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"

#include <iostream>

int main(int argc, char const* argv[])
{
  auto ml = rs::core::MusicLibrary{"."};

  ComboCommand root;

  root.addCommand<TrackCommand>("track", ml);
  root.addCommand<ListCommand>("list", ml);
  root.addCommand<InitCommand>("init", ml);
  root.addCommand<TagCommand>("tag", ml);

  try
  {
    root.execute(argc, argv, std::cout);
  }
  catch (std::exception const& e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
