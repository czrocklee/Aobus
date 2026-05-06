// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/Importer.h>

#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/tag/File.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace ao::library
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

  Importer::Importer(MusicLibrary& ml)
    : _ml{ml}
  {
  }

  void Importer::importFromYaml(std::filesystem::path const& path)
  {
    auto root = YAML::Node{};
    try
    {
      root = YAML::LoadFile(path.string());
    }
    catch (YAML::Exception const& e)
    {
      AO_THROW_FORMAT(ao::Exception, "Failed to read '{}': {}", path.string(), e.what());
    }

    if (!root["version"] || root["version"].as<int>() != 1)
    {
      AO_THROW(ao::Exception, "Unsupported YAML version");
    }

    YAML::Node library = root["library"];

    if (!library)
    {
      AO_THROW(ao::Exception, "Missing 'library' section in YAML");
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

  void Importer::importTracks(YAML::Node const& tracks,
                              ao::lmdb::WriteTransaction& txn,
                              std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId)
  {
    auto trackWriter = _ml.tracks().writer(txn);
    auto& dict = _ml.dictionary();

    for (auto const& trackNode : tracks)
    {
      std::uint32_t const yamlTrackId = trackNode["id"] ? trackNode["id"].as<std::uint32_t>() : 0;
      auto trackStrings = std::deque<std::string>{};
      std::string const uriStr = trackNode["uri"].as<std::string>();

      // 1. Try load from physical file (availability fallback)
      std::optional<TrackBuilder> optFileBuilder;
      auto const fullPath = _ml.rootPath() / uriStr;

      if (std::filesystem::exists(fullPath))
      {
        if (auto tagFile = ao::tag::File::open(fullPath); tagFile != nullptr)
        {
          optFileBuilder = tagFile->loadTrack();
          optFileBuilder->property().uri(uriStr);
          optFileBuilder->property().fileSize(std::filesystem::file_size(fullPath));
          optFileBuilder->property().mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::filesystem::last_write_time(fullPath).time_since_epoch())
                                             .count());
        }
      }

      // 2. Initialize builder
      auto builder = optFileBuilder ? *optFileBuilder : TrackBuilder::createNew();

      if (!optFileBuilder)
      {
        builder.property().uri(uriStr);
        // If no file exists, we still try to import metadata from YAML,
        // but technical properties will be empty unless provided in YAML.
      }

      // 3. Overlay YAML data (Priority: YAML > File)
      overlayMetadata(builder, trackNode, trackStrings);
      overlayCustomData(builder, trackNode, trackStrings);
      overlayTechnicalProperties(builder, trackNode);

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

  void Importer::overlayMetadata(TrackBuilder& builder,
                                 YAML::Node const& trackNode,
                                 std::deque<std::string>& trackStrings) const
  {
    auto const keepAlive = [&](YAML::Node const& node) -> std::string_view
    {
      if (!node)
      {
        return {};
      }

      return trackStrings.emplace_back(node.as<std::string>());
    };

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
  }

  void Importer::overlayCustomData(TrackBuilder& builder,
                                   YAML::Node const& trackNode,
                                   std::deque<std::string>& trackStrings) const
  {
    auto const keepAlive = [&](YAML::Node const& node) -> std::string_view
    {
      if (!node)
      {
        return {};
      }

      return trackStrings.emplace_back(node.as<std::string>());
    };

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
  }

  void Importer::overlayTechnicalProperties(TrackBuilder& builder, YAML::Node const& trackNode) const
  {
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
  }

  void Importer::importLists(YAML::Node const& lists,
                             ao::lmdb::WriteTransaction& txn,
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
        AO_THROW(ao::Exception, "List id 0 is reserved for the root");
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
            AO_THROW_FORMAT(
              ao::Exception, "List '{}' contains invalid track reference (expected ID)", importedList.name);
          }

          if (auto const it = yamlTrackIdToInternalId.find(yamlId); it != yamlTrackIdToInternalId.end())
          {
            importedList.trackIds.push_back(it->second);
          }
          else
          {
            AO_THROW_FORMAT(ao::Exception, "List '{}' references unknown track ID {}", importedList.name, yamlId);
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
        AO_THROW_FORMAT(ao::Exception, "Duplicate list id {} in YAML import", importedList.yamlId);
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
        AO_THROW_FORMAT(
          ao::Exception, "List '{}' references missing parent id {}", importedList.name, importedList.yamlParentId);
      }

      listWriter.update(childIt->second, serializeList(importedList, parentIt->second));
    }
  }
} // namespace ao::library
