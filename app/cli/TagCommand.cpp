// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TagCommand.h"
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>

#include <algorithm>
#include <ranges>
#include <sstream>

namespace ao::tool
{
  namespace
  {
    using namespace ao::library;

    void addTag(MusicLibrary& ml, TrackId trackId, std::string const& tagName, std::ostream& os)
    {
      auto txn = ml.writeTransaction();
      auto writer = ml.tracks().writer(txn);
      auto optTrackView = writer.get(trackId, TrackStore::Reader::LoadMode::Hot);

      if (!optTrackView)
      {
        os << "error: track not found: " << trackId << '\n';
        return;
      }

      auto builder = TrackBuilder::fromView(*optTrackView, ml.dictionary());

      // Check if tag already exists by iterating tag names

      if (std::ranges::contains(builder.tags().names(), tagName))
      {
        os << "tag already exists: " << tagName << '\n';
        return;
      }

      builder.tags().add(tagName);

      auto hotData = builder.serializeHot(txn, ml.dictionary());
      writer.updateHot(trackId, hotData);
      txn.commit();

      os << "added tag: " << tagName << " to track " << trackId << '\n';
    }

    void removeTag(MusicLibrary& ml, TrackId trackId, std::string const& tagName, std::ostream& os)
    {
      auto txn = ml.writeTransaction();
      auto writer = ml.tracks().writer(txn);
      auto optTrackView = writer.get(trackId, TrackStore::Reader::LoadMode::Hot);

      if (!optTrackView)
      {
        os << "error: track not found: " << trackId << '\n';
        return;
      }

      auto builder = TrackBuilder::fromView(*optTrackView, ml.dictionary());

      if (!std::ranges::contains(builder.tags().names(), tagName))
      {
        os << "tag not found on track: " << tagName << '\n';
        return;
      }

      builder.tags().remove(tagName);

      auto hotData = builder.serializeHot(txn, ml.dictionary());
      writer.updateHot(trackId, hotData);
      txn.commit();

      os << "removed tag: " << tagName << " from track " << trackId << '\n';
    }

    void showTags(MusicLibrary& ml, TrackId trackId, std::ostream& os)
    {
      auto txn = ml.readTransaction();
      auto reader = ml.tracks().reader(txn);
      auto optTrackView = reader.get(trackId, TrackStore::Reader::LoadMode::Hot);

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

  void setupTagCommand(CLI::App& app, ao::library::MusicLibrary& ml)
  {
    auto* tag = app.add_subcommand("tag", "Tag management commands");

    auto* add = tag->add_subcommand("add", "Add a tag to a track");
    auto* addId = add->add_option("id", "track id")->required();
    auto* addTagName = add->add_option("tag", "tag name")->required();
    add->callback([&ml, addId, addTagName]()
                  { addTag(ml, ao::TrackId{addId->as<std::uint32_t>()}, addTagName->as<std::string>(), std::cout); });

    auto* remove = tag->add_subcommand("remove", "Remove a tag from a track");
    auto* remId = remove->add_option("id", "track id")->required();
    auto* remTagName = remove->add_option("tag", "tag name")->required();
    remove->callback(
      [&ml, remId, remTagName]()
      { removeTag(ml, ao::TrackId{remId->as<std::uint32_t>()}, remTagName->as<std::string>(), std::cout); });

    auto* show = tag->add_subcommand("show", "Show tags for a track");
    auto* showId = show->add_option("id", "track id")->required();
    show->callback([&ml, showId]() { showTags(ml, ao::TrackId{showId->as<std::uint32_t>()}, std::cout); });
  }
}
