// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TagCommand.h"
#include "BasicCommand.h"
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackStore.h>
#include <rs/core/DictionaryStore.h>

#include <algorithm>
#include <sstream>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  void addTag(core::MusicLibrary& ml, core::TrackId trackId, std::string const& tagName, std::ostream& os)
  {
    auto txn = ml.writeTransaction();
    auto writer = ml.tracks().writer(txn);

    auto optTrackView = writer.getCold(trackId);
    if (!optTrackView)
    {
      os << "error: track not found: " << trackId << std::endl;
      return;
    }

    core::TrackRecord record(*optTrackView, ml.dictionary());

    auto tagId = ml.dictionary().getId(tagName);
    bool tagExists = false;
    for (auto existingTagId : record.tags.ids)
    {
      if (existingTagId == tagId)
      {
        tagExists = true;
        break;
      }
    }

    if (tagExists)
    {
      os << "tag already exists: " << tagName << std::endl;
      txn.commit();
      return;
    }

    if (tagId.value() == 0)
    {
      tagId = ml.dictionary().put(txn, tagName);
    }

    record.tags.ids.push_back(tagId);

    auto hotData = record.serializeHot();
    (void)writer.updateHot(trackId, hotData);
    txn.commit();

    os << "added tag: " << tagName << " to track " << trackId << std::endl;
  }

  void removeTag(core::MusicLibrary& ml, core::TrackId trackId, std::string const& tagName, std::ostream& os)
  {
    auto txn = ml.writeTransaction();
    auto writer = ml.tracks().writer(txn);

    auto optTrackView = writer.getCold(trackId);
    if (!optTrackView)
    {
      os << "error: track not found: " << trackId << std::endl;
      return;
    }

    core::TrackRecord record(*optTrackView, ml.dictionary());

    auto tagId = ml.dictionary().getId(tagName);
    if (tagId.value() == 0)
    {
      os << "tag not found: " << tagName << std::endl;
      return;
    }

    auto& tags = record.tags.ids;
    auto it = std::remove(tags.begin(), tags.end(), tagId);
    if (it == tags.end())
    {
      os << "tag not found on track: " << tagName << std::endl;
      return;
    }
    tags.erase(it, tags.end());

    auto hotData = record.serializeHot();
    (void)writer.updateHot(trackId, hotData);
    txn.commit();

    os << "removed tag: " << tagName << " from track " << trackId << std::endl;
  }

  void showTags(core::MusicLibrary& ml, core::TrackId trackId, std::ostream& os)
  {
    auto txn = ml.readTransaction();
    auto reader = ml.tracks().reader(txn);

    auto optTrackView = reader.get(trackId);
    if (!optTrackView)
    {
      os << "error: track not found: " << trackId << std::endl;
      return;
    }

    core::TrackRecord record(*optTrackView, ml.dictionary());

    if (record.tags.ids.empty())
    {
      os << "no tags" << std::endl;
      return;
    }

    os << "tags: ";
    bool first = true;
    for (auto tagId : record.tags.ids)
    {
      if (!first) os << ", ";
      os << ml.dictionary().get(core::DictionaryId{tagId});
      first = false;
    }
    os << std::endl;
  }
}

TagCommand::TagCommand(rs::core::MusicLibrary& ml) : _ml{ml}
{
  addCommand<BasicCommand>("add")
    .addOption("id", bpo::value<std::uint64_t>()->required(), "track id", 1)
    .addOption("tag", bpo::value<std::string>()->required(), "tag name", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = rs::core::TrackId{vm["id"].template as<std::uint32_t>()};
      auto tag = vm["tag"].template as<std::string>();
      addTag(_ml, id, tag, os);
      return "";
    });

  addCommand<BasicCommand>("remove")
    .addOption("id", bpo::value<std::uint64_t>()->required(), "track id", 1)
    .addOption("tag", bpo::value<std::string>()->required(), "tag name", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = rs::core::TrackId{vm["id"].template as<std::uint32_t>()};
      auto tag = vm["tag"].template as<std::string>();
      removeTag(_ml, id, tag, os);
      return "";
    });

  addCommand<BasicCommand>("show")
    .addOption("id", bpo::value<std::uint64_t>()->required(), "track id", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = rs::core::TrackId{vm["id"].template as<std::uint32_t>()};
      showTags(_ml, id, os);
      return "";
    });
}