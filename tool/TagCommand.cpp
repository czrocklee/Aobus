// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TagCommand.h"
#include "BasicCommand.h"
#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackStore.h>

#include <algorithm>
#include <ranges>
#include <sstream>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs::core;

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

    auto record = TrackRecord{*optTrackView, ml.dictionary()};
    auto tagId = ml.dictionary().put(txn, tagName);

    if (std::ranges::any_of(record.tags.ids, [tagId](auto tag) { return tag == tagId; }))
    {
      os << "tag already exists: " << tagName << '\n';
      return;
    }

    record.tags.ids.push_back(tagId);

    auto hotData = record.serializeHot();
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

    auto record = TrackRecord{*optTrackView, ml.dictionary()};
    auto tagId = ml.dictionary().getId(tagName);

    if (auto erased = std::erase(record.tags.ids, tagId); erased == 0)
    {
      os << "tag not found on track: " << tagName << '\n';
      return;
    }

    auto hotData = record.serializeHot();
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

    auto record = TrackRecord{*optTrackView, ml.dictionary()};

    if (record.tags.ids.empty())
    {
      os << "no tags" << '\n';
      return;
    }

    os << "tags: ";
    auto tags = record.tags.ids | std::ranges::views::transform([&](auto id) { return ml.dictionary().get(id); });
    os << std::format("{}\n", tags);
  }
}

TagCommand::TagCommand(MusicLibrary& ml)
  : _ml{ml}
{
  addCommand<BasicCommand>("add")
    .addOption("id", bpo::value<std::uint32_t>()->required(), "track id", 1)
    .addOption("tag", bpo::value<std::string>()->required(), "tag name", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = TrackId{vm["id"].template as<std::uint32_t>()};
      auto tag = vm["tag"].template as<std::string>();
      addTag(_ml, id, tag, os);
    });

  addCommand<BasicCommand>("remove")
    .addOption("id", bpo::value<std::uint32_t>()->required(), "track id", 1)
    .addOption("tag", bpo::value<std::string>()->required(), "tag name", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = TrackId{vm["id"].template as<std::uint32_t>()};
      auto tag = vm["tag"].template as<std::string>();
      removeTag(_ml, id, tag, os);
    });

  addCommand<BasicCommand>("show")
    .addOption("id", bpo::value<std::uint32_t>()->required(), "track id", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = TrackId{vm["id"].template as<std::uint32_t>()};
      showTags(_ml, id, os);
    });
}