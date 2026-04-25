// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/MusicLibrary.h>

#include "ComboCommand.h"
#include "InitCommand.h"
#include "LibCommand.h"
#include "ListCommand.h"
#include "TagCommand.h"
#include "TrackCommand.h"

#include <iostream>

int main(int argc, char const* argv[])
{
  auto ml = rs::core::MusicLibrary{"."};

  auto root = ComboCommand{};

  root.addCommand<TrackCommand>("track", ml);
  root.addCommand<ListCommand>("list", ml);
  root.addCommand<InitCommand>("init", ml);
  root.addCommand<TagCommand>("tag", ml);
  root.addCommand<LibCommand>("lib", ml);

  try
  {
    root.execute(argc, argv, std::cout);
  }
  catch (std::exception const& e)
  {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
