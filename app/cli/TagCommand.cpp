// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TagCommand.h"

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/TrackCommandService.h>

#include <CLI/App.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace ao::cli
{
  namespace
  {
    using namespace ao::library;

    void showTags(MusicLibrary& ml, TrackId trackId, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);
      auto const optTrackView = reader.get(trackId, TrackStore::Reader::LoadMode::Hot);

      if (!optTrackView)
      {
        os << "error: track not found: " << trackId << '\n';
        return;
      }

      auto builder = TrackBuilder::fromView(*optTrackView, ml.dictionary());
      auto const& tagNames = builder.tags().names();

      if (tagNames.empty())
      {
        os << "no tags" << '\n';
        return;
      }

      os << "tags: ";

      for (auto const& name : tagNames)
      {
        os << name << " ";
      }

      os << '\n';
    }
  }

  void setupTagCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* tag = app.add_subcommand("tag", "Tag management commands");

    auto* add = tag->add_subcommand("add", "Add a tag to a track");
    auto* addId = add->add_option("id", "track id")->required();
    auto* addTagName = add->add_option("tag", "tag name")->required();
    add->callback(
      [&runtime, addId, addTagName]
      {
        if (runtime.trackCommands().addTag(TrackId{addId->as<std::uint32_t>()}, addTagName->as<std::string>()))
        {
          std::cout << "added tag: " << addTagName->as<std::string>() << " to track " << addId->as<std::uint32_t>()
                    << '\n';
        }
        else
        {
          std::cout << "error adding tag (track not found or tag already exists)\n";
        }
      });

    auto* remove = tag->add_subcommand("remove", "Remove a tag from a track");
    auto* remId = remove->add_option("id", "track id")->required();
    auto* remTagName = remove->add_option("tag", "tag name")->required();
    remove->callback(
      [&runtime, remId, remTagName]
      {
        if (runtime.trackCommands().removeTag(TrackId{remId->as<std::uint32_t>()}, remTagName->as<std::string>()))
        {
          std::cout << "removed tag: " << remTagName->as<std::string>() << " from track " << remId->as<std::uint32_t>()
                    << '\n';
        }
        else
        {
          std::cout << "error removing tag (track not found or tag missing)\n";
        }
      });

    auto* show = tag->add_subcommand("show", "Show tags for a track");
    auto* showId = show->add_option("id", "track id")->required();
    show->callback([&runtime, showId]
                   { showTags(runtime.musicLibrary(), TrackId{showId->as<std::uint32_t>()}, std::cout); });
  }
}
