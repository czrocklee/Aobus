// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/LibraryImporter.h>

#include <rs/Exception.h>
#include <rs/core/DictionaryStore.h>
#include <rs/core/ListBuilder.h>
#include <rs/core/ListStore.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackBuilder.h>
#include <rs/core/TrackStore.h>
#include <rs/tag/File.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace rs::core
{

  namespace
  {
    struct ImportedList
    {
      std::uint32_t yamlId = 0;
      std::uint32_t yamlParentId = 0;
      std::string name;
      std::string description;
      std::string filter;
      std::vector<TrackId> trackIds;
      bool isSmart = false;
    };

    std::vector<std::byte> serializeList(ImportedList const& list, ListId parentId)
    {
      auto builder = ListBuilder::createNew().name(list.name).description(list.description).parentId(parentId);

      if (list.isSmart)
      {
        builder.filter(list.filter);
      }
      else
      {
        for (auto const trackId : list.trackIds)
        {
          builder.tracks().add(trackId);
        }
      }

      return builder.serialize();
    }
  }

  LibraryImporter::LibraryImporter(MusicLibrary& ml)
    : _ml{ml}
  {
  }

  void LibraryImporter::importFromYaml(std::filesystem::path const& path)
  {
    auto root = YAML::Node{};
    try
    {
      root = YAML::LoadFile(path.string());
    }
    catch (YAML::Exception const& e)
    {
      RS_THROW_FORMAT(rs::Exception, "Failed to read '{}': {}", path.string(), e.what());
    }

    if (!root["version"] || root["version"].as<int>() != 1)
    {
      RS_THROW(rs::Exception, "Unsupported YAML version");
    }

    YAML::Node library = root["library"];

    if (!library)
    {
      RS_THROW(rs::Exception, "Missing 'library' section in YAML");
    }

    auto txn = _ml.writeTransaction();

    // Clear existing data for a fresh import
    _ml.tracks().writer(txn).clear();
    _ml.lists().writer(txn).clear();

    auto yamlTrackIdToInternalId = std::unordered_map<std::uint32_t, TrackId>{};

    if (library["tracks"])
    {
      importTracks(library["tracks"], txn, yamlTrackIdToInternalId);
    }

    if (library["lists"])
    {
      importLists(library["lists"], txn, yamlTrackIdToInternalId);
    }

    txn.commit();
  }

  void LibraryImporter::importTracks(YAML::Node const& tracks,
                                     rs::lmdb::WriteTransaction& txn,
                                     std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId)
  {
    auto trackWriter = _ml.tracks().writer(txn);
    auto& dict = _ml.dictionary();

    for (auto const& trackNode : tracks)
    {
      std::uint32_t yamlTrackId = trackNode["id"] ? trackNode["id"].as<std::uint32_t>() : 0;
      auto trackStrings = std::deque<std::string>{};
      auto keepAlive = [&](YAML::Node const& node) -> std::string_view
      {
        if (!node)
        {
          return {};
        }

        return trackStrings.emplace_back(node.as<std::string>());
      };

      std::string uriStr = trackNode["uri"].as<std::string>();

      // 1. Try load from physical file (availability fallback)
      std::optional<TrackBuilder> fileBuilder;
      auto fullPath = _ml.rootPath() / uriStr;
      auto tagFile = std::unique_ptr<rs::tag::File>{};

      if (std::filesystem::exists(fullPath))
      {
        tagFile = rs::tag::File::open(fullPath);

        if (tagFile)
        {
          fileBuilder = tagFile->loadTrack();
          fileBuilder->property().uri(uriStr);
          fileBuilder->property().fileSize(std::filesystem::file_size(fullPath));
          fileBuilder->property().mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::filesystem::last_write_time(fullPath).time_since_epoch())
                                          .count());
        }
      }

      // 2. Initialize builder
      auto builder = fileBuilder ? *fileBuilder : TrackBuilder::createNew();

      if (!fileBuilder)
      {
        builder.property().uri(uriStr);
        // If no file exists, we still try to import metadata from YAML,
        // but technical properties will be empty unless provided in YAML.
      }

      // 3. Overlay YAML data (Priority: YAML > File)

      if (trackNode["title"])
      {
        builder.metadata().title(keepAlive(trackNode["title"]));
      }

      if (trackNode["artist"])
      {
        builder.metadata().artist(keepAlive(trackNode["artist"]));
      }

      if (trackNode["album"])
      {
        builder.metadata().album(keepAlive(trackNode["album"]));
      }

      if (trackNode["albumArtist"])
      {
        builder.metadata().albumArtist(keepAlive(trackNode["albumArtist"]));
      }

      if (trackNode["genre"])
      {
        builder.metadata().genre(keepAlive(trackNode["genre"]));
      }

      if (trackNode["year"])
      {
        builder.metadata().year(trackNode["year"].as<uint16_t>());
      }

      if (trackNode["trackNumber"])
      {
        builder.metadata().trackNumber(trackNode["trackNumber"].as<uint16_t>());
      }

      if (trackNode["totalTracks"])
      {
        builder.metadata().totalTracks(trackNode["totalTracks"].as<uint16_t>());
      }

      if (trackNode["discNumber"])
      {
        builder.metadata().discNumber(trackNode["discNumber"].as<uint16_t>());
      }

      if (trackNode["totalDiscs"])
      {
        builder.metadata().totalDiscs(trackNode["totalDiscs"].as<uint16_t>());
      }

      if (trackNode["rating"])
      {
        builder.metadata().rating(trackNode["rating"].as<uint8_t>());
      }

      if (trackNode["tags"])
      {
        builder.tags().clear();
        for (auto const& tag : trackNode["tags"])
        {
          builder.tags().add(keepAlive(tag));
        }
      }

      if (trackNode["custom"])
      {
        builder.custom().clear();

        for (auto it = trackNode["custom"].begin(); it != trackNode["custom"].end(); ++it)
        {
          builder.custom().add(keepAlive(it->first), keepAlive(it->second));
        }
      }

      // YAML technical properties (optional override)

      if (trackNode["durationMs"])
      {
        builder.property().durationMs(trackNode["durationMs"].as<uint32_t>());
      }

      if (trackNode["bitrate"])
      {
        builder.property().bitrate(trackNode["bitrate"].as<uint32_t>());
      }

      if (trackNode["sampleRate"])
      {
        builder.property().sampleRate(trackNode["sampleRate"].as<uint32_t>());
      }

      if (trackNode["channels"])
      {
        builder.property().channels(trackNode["channels"].as<uint8_t>());
      }

      if (trackNode["bitDepth"])
      {
        builder.property().bitDepth(trackNode["bitDepth"].as<uint8_t>());
      }

      if (trackNode["codecId"])
      {
        builder.property().codecId(trackNode["codecId"].as<uint16_t>());
      }

      // 4. Commit as new track
      auto [preparedHot, preparedCold] = builder.prepare(txn, dict, _ml.resources());
      auto [newTrackId, view] =
        trackWriter.createHotCold(preparedHot.size(),
                                  preparedCold.size(),
                                  [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                  {
                                    preparedHot.writeTo(hot);
                                    preparedCold.writeTo(cold);
                                  });
      std::ignore = view;

      if (yamlTrackId != 0)
      {
        yamlTrackIdToInternalId[yamlTrackId] = newTrackId;
      }
    }
  }

  void LibraryImporter::importLists(YAML::Node const& lists,
                                    rs::lmdb::WriteTransaction& txn,
                                    std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId)
  {
    auto listWriter = _ml.lists().writer(txn);
    auto importedLists = std::vector<ImportedList>{};
    importedLists.reserve(lists.size());

    for (auto const& listNode : lists)
    {
      auto importedList = ImportedList{};
      importedList.yamlId = listNode["id"].as<uint32_t>();
      importedList.yamlParentId = listNode["parentId"].as<uint32_t>();
      importedList.name = listNode["name"].as<std::string>();

      if (importedList.yamlId == 0)
      {
        RS_THROW(rs::Exception, "List id 0 is reserved for the root");
      }

      if (listNode["description"])
      {
        importedList.description = listNode["description"].as<std::string>();
      }

      if (listNode["filter"])
      {
        importedList.isSmart = true;
        importedList.filter = listNode["filter"].as<std::string>();
      }
      else if (listNode["tracks"])
      {
        importedList.trackIds.reserve(listNode["tracks"].size());
        for (auto const& trackRefNode : listNode["tracks"])
        {
          std::uint32_t yamlId = 0;
          try
          {
            yamlId = trackRefNode.as<std::uint32_t>();
          }
          catch (...)
          {
            RS_THROW_FORMAT(
              rs::Exception, "List '{}' contains invalid track reference (expected ID)", importedList.name);
          }

          if (auto const it = yamlTrackIdToInternalId.find(yamlId); it != yamlTrackIdToInternalId.end())
          {
            importedList.trackIds.push_back(it->second);
          }
          else
          {
            RS_THROW_FORMAT(rs::Exception, "List '{}' references unknown track ID {}", importedList.name, yamlId);
          }
        }
      }

      importedLists.push_back(std::move(importedList));
    }

    std::unordered_map<std::uint32_t, ListId> yamlListIdToNewListId;
    yamlListIdToNewListId.reserve(importedLists.size());
    for (auto const& importedList : importedLists)
    {
      auto const [mappingIt, inserted] = yamlListIdToNewListId.emplace(importedList.yamlId, ListId{});

      if (!inserted)
      {
        RS_THROW_FORMAT(rs::Exception, "Duplicate list id {} in YAML import", importedList.yamlId);
      }

      auto [newListId, view] = listWriter.create(serializeList(importedList, ListId{0}));
      std::ignore = view;
      mappingIt->second = newListId;
    }

    for (auto const& importedList : importedLists)
    {
      if (importedList.yamlParentId == 0)
      {
        continue;
      }

      auto const childIt = yamlListIdToNewListId.find(importedList.yamlId);
      auto const parentIt = yamlListIdToNewListId.find(importedList.yamlParentId);

      if (parentIt == yamlListIdToNewListId.end())
      {
        RS_THROW_FORMAT(
          rs::Exception, "List '{}' references missing parent id {}", importedList.name, importedList.yamlParentId);
      }

      listWriter.update(childIt->second, serializeList(importedList, parentIt->second));
    }
  }

} // namespace rs::core
