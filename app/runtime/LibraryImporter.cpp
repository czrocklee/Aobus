// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/LibraryImporter.h"

#include "ao/Exception.h"
#include "ao/Type.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/ListBuilder.h"
#include "ao/library/ListStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/lmdb/Transaction.h"
#include "ao/tag/TagFile.h"
#include "ao/utility/Base64.h"
#include "runtime/LibraryExporter.h"
#include "runtime/TrackField.h"

#include <yaml-cpp/yaml.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct ValidatedTrack final
    {
      std::uint32_t yamlId = 0;
      std::string uri;
      YAML::Node node;
    };

    struct ValidatedList final
    {
      std::uint32_t yamlId = 0;
      std::uint32_t yamlParentId = 0;
      std::string name;
      std::string description;
      std::string filter;
      std::vector<std::uint32_t> yamlTrackIds;
      std::vector<std::string> trackUris;
      bool isSmart = false;
    };

    struct ValidatedImport final
    {
      std::uint32_t version = 0;
      ExportMode payloadMode = ExportMode::Full;
      std::optional<std::string> libraryId;
      std::vector<ValidatedTrack> tracks;
      std::vector<ValidatedList> lists;
    };

    ExportMode parseExportMode(std::string_view modeStr)
    {
      if (modeStr == "delta" || modeStr == "minimum")
      {
        return ExportMode::Delta;
      }

      if (modeStr == "metadata")
      {
        return ExportMode::Metadata;
      }

      if (modeStr == "full")
      {
        return ExportMode::Full;
      }

      if (modeStr == "listOnly")
      {
        return ExportMode::ListOnly;
      }

      return ExportMode::Full;
    }

    constexpr std::uint8_t hexDigit(char ch)
    {
      if (ch >= '0' && ch <= '9')
      {
        return static_cast<std::uint8_t>(ch - '0');
      }

      constexpr std::uint8_t kHexBase = 10;

      if (ch >= 'a' && ch <= 'f')
      {
        return static_cast<std::uint8_t>(ch - 'a' + kHexBase);
      }

      if (ch >= 'A' && ch <= 'F')
      {
        return static_cast<std::uint8_t>(ch - 'A' + kHexBase);
      }

      return 0;
    }

    std::array<std::byte, 16> parseUuid(std::string_view uuidStr)
    {
      auto result = std::array<std::byte, 16>{};
      auto byteIndex = 0;

      for (std::size_t i = 0; i < uuidStr.size() && byteIndex < 16; ++i)
      {
        if (uuidStr[i] == '-')
        {
          continue;
        }

        if (i + 1 < uuidStr.size())
        {
          auto const hi = hexDigit(uuidStr[i]);
          auto const lo = hexDigit(uuidStr[i + 1]);
          result.at(byteIndex++) = static_cast<std::byte>((hi << 4U) | lo);
          ++i;
        }
      }

      return result;
    }
  }

  struct LibraryImporter::Impl final
  {
    explicit Impl(library::MusicLibrary& ml)
      : ml{ml}
    {
    }

    void importFromYaml(std::filesystem::path const& path, ImportMode mode);

    ValidatedImport validate(YAML::Node const& root) const;
    void validateTracks(YAML::Node const& tracks, ValidatedImport& validated) const;
    void validateLists(YAML::Node const& lists, ValidatedImport& validated) const;

    void importTracks(std::vector<ValidatedTrack> const& tracks,
                      lmdb::WriteTransaction& txn,
                      std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                      ImportMode strategy,
                      ExportMode payloadMode);
    void loadTrackBaseline(std::string const& uriStr,
                           std::optional<TrackId> const& optExistingTrackId,
                           ExportMode payloadMode,
                           std::optional<library::TrackBuilder>& optBuilder,
                           std::unique_ptr<tag::TagFile>& keepAliveTagFile,
                           library::TrackStore::Writer& trackWriter);

    void importLists(std::vector<ValidatedList> const& lists,
                     lmdb::WriteTransaction& txn,
                     std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                     ImportMode strategy);

    void overlayMetadata(library::TrackBuilder& builder,
                         YAML::Node const& trackNode,
                         std::deque<std::string>& trackStrings) const;
    void overlayCustomData(library::TrackBuilder& builder,
                           YAML::Node const& trackNode,
                           std::deque<std::string>& trackStrings) const;
    void overlayTechnicalProperties(library::TrackBuilder& builder, YAML::Node const& trackNode) const;

    void loadFileBaseline(std::string const& uriStr,
                          ExportMode payloadMode,
                          std::optional<library::TrackBuilder>& optBuilder,
                          std::unique_ptr<tag::TagFile>& keepAliveTagFile) const;

    library::MusicLibrary& ml;
  };

  LibraryImporter::LibraryImporter(library::MusicLibrary& ml)
    : _impl{std::make_unique<Impl>(ml)}
  {
  }

  LibraryImporter::~LibraryImporter() = default;

  void LibraryImporter::importFromYaml(std::filesystem::path const& path, ImportMode mode)
  {
    _impl->importFromYaml(path, mode);
  }

  void LibraryImporter::Impl::importFromYaml(std::filesystem::path const& path, ImportMode mode)
  {
    auto root = YAML::Node{};

    try
    {
      root = YAML::LoadFile(path.string());
    }
    catch (YAML::Exception const& e)
    {
      ao::throwException<Exception>("Failed to read '{}': {}", path.string(), e.what());
    }

    auto const validated = validate(root);

    auto txn = ml.writeTransaction();

    if (mode == ImportMode::Restore)
    {
      if (validated.payloadMode != ExportMode::ListOnly)
      {
        ml.tracks().writer(txn).clear();
        ml.manifest().writer(txn).clear();
      }

      ml.lists().writer(txn).clear();
    }

    auto yamlTrackIdToInternalId = std::unordered_map<std::uint32_t, TrackId>{};

    if (!validated.tracks.empty())
    {
      importTracks(validated.tracks, txn, yamlTrackIdToInternalId, mode, validated.payloadMode);
    }

    if (!validated.lists.empty())
    {
      importLists(validated.lists, txn, yamlTrackIdToInternalId, mode);
    }

    // Commit the main import transaction before updating the meta header.
    // updateMetaHeader creates its own WriteTransaction, and LMDB does not
    // support nested write transactions on the same thread.
    txn.commit();

    if (mode == ImportMode::Restore && validated.libraryId)
    {
      auto header = ml.metaHeader();
      header.libraryId = parseUuid(*validated.libraryId);
      ml.updateMetaHeader(header);
    }
  }

  ValidatedImport LibraryImporter::Impl::validate(YAML::Node const& root) const
  {
    auto validated = ValidatedImport{};

    if (!root["version"])
    {
      ao::throwException<Exception>("Missing 'version' field in YAML");
    }

    validated.version = root["version"].as<uint32_t>();

    if (validated.version != 1)
    {
      ao::throwException<Exception>("Unsupported YAML version {}", validated.version);
    }

    if (root["export_mode"])
    {
      validated.payloadMode = parseExportMode(root["export_mode"].as<std::string>());
    }
    else
    {
      validated.payloadMode = ExportMode::Full;
    }

    if (root["libraryId"])
    {
      validated.libraryId = root["libraryId"].as<std::string>();
    }

    auto const library = root["library"];

    if (!library)
    {
      ao::throwException<Exception>("Missing 'library' section in YAML");
    }

    if (validated.payloadMode != ExportMode::ListOnly)
    {
      if (auto const tracks = library["tracks"]; tracks)
      {
        validateTracks(tracks, validated);
      }
    }

    if (auto const lists = library["lists"]; lists)
    {
      validateLists(lists, validated);
    }

    return validated;
  }

  void LibraryImporter::Impl::validateTracks(YAML::Node const& tracks, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (YAML::const_iterator it = tracks.begin(); it != tracks.end(); ++it)
    {
      auto trackNode = YAML::Node{*it};
      auto track = ValidatedTrack{};

      if (!trackNode["uri"])
      {
        ao::throwException<Exception>("Track record missing required 'uri' field");
      }

      track.uri = trackNode["uri"].as<std::string>();

      if (track.uri.empty())
      {
        ao::throwException<Exception>("Track record has empty 'uri'");
      }

      if (trackNode["id"])
      {
        track.yamlId = trackNode["id"].as<uint32_t>();

        if (track.yamlId != 0)
        {
          if (seenYamlIds.contains(track.yamlId))
          {
            ao::throwException<Exception>("Duplicate track YAML id {} in payload", track.yamlId);
          }

          seenYamlIds.insert(track.yamlId);
        }
      }

      track.node = trackNode;
      validated.tracks.push_back(std::move(track));
    }
  }

  void LibraryImporter::Impl::validateLists(YAML::Node const& lists, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (YAML::const_iterator it = lists.begin(); it != lists.end(); ++it)
    {
      auto const& listNode = *it;
      auto list = ValidatedList{};

      if (!listNode["id"])
      {
        ao::throwException<Exception>("List record missing required 'id' field");
      }

      list.yamlId = listNode["id"].as<uint32_t>();

      if (list.yamlId == 0)
      {
        ao::throwException<Exception>("List id 0 is reserved for the root");
      }

      if (seenYamlIds.contains(list.yamlId))
      {
        ao::throwException<Exception>("Duplicate list YAML id {} in payload", list.yamlId);
      }

      seenYamlIds.insert(list.yamlId);

      if (!listNode["name"])
      {
        ao::throwException<Exception>("List record missing required 'name' field");
      }

      list.name = listNode["name"].as<std::string>();
      list.yamlParentId = listNode["parentId"] ? listNode["parentId"].as<uint32_t>() : 0;

      if (listNode["description"])
      {
        list.description = listNode["description"].as<std::string>();
      }

      if (listNode["filter"])
      {
        list.isSmart = true;
        list.filter = listNode["filter"].as<std::string>();
      }
      else if (listNode["tracks"])
      {
        for (auto const& trackRef : listNode["tracks"])
        {
          if (trackRef.IsScalar())
          {
            list.yamlTrackIds.push_back(trackRef.as<uint32_t>());
          }
          else if (trackRef.IsMap())
          {
            if (trackRef["id"])
            {
              list.yamlTrackIds.push_back(trackRef["id"].as<uint32_t>());
            }
            else if (trackRef["uri"])
            {
              list.trackUris.push_back(trackRef["uri"].as<std::string>());
            }
          }
        }
      }

      validated.lists.push_back(std::move(list));
    }
  }

  void LibraryImporter::Impl::importTracks(std::vector<ValidatedTrack> const& tracks,
                                           lmdb::WriteTransaction& txn,
                                           std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                                           ImportMode strategy,
                                           ExportMode payloadMode)
  {
    auto trackWriter = ml.tracks().writer(txn);
    auto manifestWriter = ml.manifest().writer(txn);
    auto manifestReader = ml.manifest().reader(txn);
    auto& dict = ml.dictionary();
    auto& resources = ml.resources();

    for (auto const& validatedTrack : tracks)
    {
      auto const& trackNode = validatedTrack.node;
      auto trackStrings = std::deque<std::string>{};
      std::string const& uriStr = validatedTrack.uri;

      // URI-based matching for Merge mode
      auto optExistingTrackId = std::optional<TrackId>{};

      if (strategy == ImportMode::Merge)
      {
        if (auto const optEntry = manifestReader.get(uriStr))
        {
          optExistingTrackId = optEntry->trackId;
        }
      }

      // 1. Try load baseline
      auto optBuilder = std::optional<library::TrackBuilder>{};
      auto keepAliveTagFile = std::unique_ptr<tag::TagFile>{};

      loadTrackBaseline(uriStr, optExistingTrackId, payloadMode, optBuilder, keepAliveTagFile, trackWriter);

      // 2. Initialize builder if still empty
      auto builder = optBuilder ? *optBuilder : library::TrackBuilder::createNew();

      if (!optBuilder)
      {
        builder.property().uri(uriStr);
      }

      // 3. Overlay YAML data (Priority: YAML > File/DB)
      overlayMetadata(builder, trackNode, trackStrings);
      overlayCustomData(builder, trackNode, trackStrings);
      overlayTechnicalProperties(builder, trackNode);

      auto coverArtBinary = std::vector<std::byte>{}; // NOLINT(aobus-readability-use-if-init-statement)

      if (trackNode["coverArtBase64"])
      {
        auto const b64 = trackNode["coverArtBase64"].as<std::string>();
        coverArtBinary = utility::base64Decode(b64);

        if (!coverArtBinary.empty())
        {
          builder.metadata().coverArtData(coverArtBinary);
        }
      }

      // 4. Commit
      auto [preparedHot, preparedCold] = builder.prepare(txn, dict, resources);
      auto targetTrackId = TrackId{};

      if (optExistingTrackId)
      {
        targetTrackId = *optExistingTrackId;
        trackWriter.updateHot(
          targetTrackId, preparedHot.size(), [&](std::span<std::byte> hot) { preparedHot.writeTo(hot); });
        trackWriter.updateCold(
          targetTrackId, preparedCold.size(), [&](std::span<std::byte> cold) { preparedCold.writeTo(cold); });
      }
      else
      {
        auto [newTrackId, view] =
          trackWriter.createHotCold(preparedHot.size(),
                                    preparedCold.size(),
                                    [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                    {
                                      preparedHot.writeTo(hot);
                                      preparedCold.writeTo(cold);
                                    });
        targetTrackId = newTrackId;
        std::ignore = view;
      }

      // 5. Populate Manifest
      auto manifestEntry = library::ManifestEntry{.trackId = targetTrackId};
      manifestEntry.fileSize(builder.property().fileSize());
      manifestEntry.mtime(builder.property().mtime());
      manifestWriter.put(uriStr, manifestEntry);

      if (validatedTrack.yamlId != 0)
      {
        yamlTrackIdToInternalId[validatedTrack.yamlId] = targetTrackId;
      }
    }
  }

  void LibraryImporter::Impl::loadTrackBaseline(std::string const& uriStr,
                                                std::optional<TrackId> const& optExistingTrackId,
                                                ExportMode payloadMode,
                                                std::optional<library::TrackBuilder>& optBuilder,
                                                std::unique_ptr<tag::TagFile>& keepAliveTagFile,
                                                library::TrackStore::Writer& trackWriter)
  {
    if (optExistingTrackId)
    {
      auto const optView = trackWriter.get(*optExistingTrackId, library::TrackStore::Reader::LoadMode::Both);

      if (optView)
      {
        optBuilder = library::TrackBuilder::fromView(*optView, ml.dictionary());
      }
    }

    if (payloadMode == ExportMode::Delta || payloadMode == ExportMode::Metadata)
    {
      loadFileBaseline(uriStr, payloadMode, optBuilder, keepAliveTagFile);
    }
  }

  void LibraryImporter::Impl::loadFileBaseline(std::string const& uriStr,
                                               ExportMode payloadMode,
                                               std::optional<library::TrackBuilder>& optBuilder,
                                               std::unique_ptr<tag::TagFile>& keepAliveTagFile) const
  {
    if (auto const fullPath = ml.rootPath() / uriStr; std::filesystem::exists(fullPath))
    {
      keepAliveTagFile = tag::TagFile::open(fullPath);

      if (keepAliveTagFile != nullptr)
      {
        if (auto fileBuilder = keepAliveTagFile->loadTrack(); !optBuilder)
        {
          optBuilder = std::move(fileBuilder);

          if (payloadMode == ExportMode::Metadata)
          {
            // Clear parsed metadata fields so they don't leak into the restored track.
            // For Metadata mode, technical props are the only baseline we want.
            optBuilder->metadata()
              .title("")
              .artist("")
              .album("")
              .albumArtist("")
              .composer("")
              .genre("")
              .work("")
              .year(0)
              .trackNumber(0)
              .totalTracks(0)
              .discNumber(0)
              .totalDiscs(0)
              .rating(0);
            optBuilder->tags().clear();
            optBuilder->custom().clear();
          }
        }
        else
        {
          // Merge physical file technical props into existing builder
          auto const& props = fileBuilder.property();
          optBuilder->property()
            .durationMs(props.durationMs())
            .bitrate(props.bitrate())
            .sampleRate(props.sampleRate())
            .codecId(props.codecId())
            .channels(props.channels())
            .bitDepth(props.bitDepth());

          if (payloadMode == ExportMode::Delta)
          {
            // For Delta mode merge, we also refresh embedded cover art from file if it wasn't modified in DB
            if (optBuilder->metadata().coverArtId() == 0)
            {
              optBuilder->metadata().coverArtData(fileBuilder.metadata().coverArtData());
            }
          }
        }

        // Always update file tracking info from physical file
        optBuilder->property().uri(uriStr);
        optBuilder->property().fileSize(std::filesystem::file_size(fullPath));
        optBuilder->property().mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::filesystem::last_write_time(fullPath).time_since_epoch())
                                       .count());
      }
    }
  }

  void LibraryImporter::Impl::overlayMetadata(library::TrackBuilder& builder,
                                              YAML::Node const& trackNode,
                                              std::deque<std::string>& trackStrings) const
  {
    auto const keepAlive = [&](YAML::Node const& node) -> std::string_view
    {
      if (!node)
      {
        return {};
      }

      return trackStrings.emplace_back(node.template as<std::string>());
    };

    using StringSetter = void (*)(library::TrackBuilder::MetadataBuilder&, std::string_view);
    using NumberSetter = void (*)(library::TrackBuilder::MetadataBuilder&, std::uint16_t);

    struct Dispatch
    {
      rt::TrackField field;
      StringSetter stringSetter;
      NumberSetter numberSetter;
    };

    constexpr auto kMetadataDispatch = std::to_array<Dispatch>({
      {.field = rt::TrackField::Title, .stringSetter = [](auto& meta, auto value) { meta.title(value); }},
      {.field = rt::TrackField::Artist, .stringSetter = [](auto& meta, auto value) { meta.artist(value); }},
      {.field = rt::TrackField::Album, .stringSetter = [](auto& meta, auto value) { meta.album(value); }},
      {.field = rt::TrackField::AlbumArtist, .stringSetter = [](auto& meta, auto value) { meta.albumArtist(value); }},
      {.field = rt::TrackField::Composer, .stringSetter = [](auto& meta, auto value) { meta.composer(value); }},
      {.field = rt::TrackField::Genre, .stringSetter = [](auto& meta, auto value) { meta.genre(value); }},
      {.field = rt::TrackField::Work, .stringSetter = [](auto& meta, auto value) { meta.work(value); }},
      {.field = rt::TrackField::Year, .numberSetter = [](auto& meta, auto value) { meta.year(value); }},
      {.field = rt::TrackField::TrackNumber, .numberSetter = [](auto& meta, auto value) { meta.trackNumber(value); }},
      {.field = rt::TrackField::TotalTracks, .numberSetter = [](auto& meta, auto value) { meta.totalTracks(value); }},
      {.field = rt::TrackField::DiscNumber, .numberSetter = [](auto& meta, auto value) { meta.discNumber(value); }},
      {.field = rt::TrackField::TotalDiscs, .numberSetter = [](auto& meta, auto value) { meta.totalDiscs(value); }},
    });

    for (auto const& map : kMetadataDispatch)
    {
      auto const key = rt::trackFieldId(map.field);

      if (auto node = trackNode[key.data()])
      {
        if (map.stringSetter != nullptr)
        {
          map.stringSetter(builder.metadata(), keepAlive(node));
        }
        else if (map.numberSetter != nullptr)
        {
          map.numberSetter(builder.metadata(), node.template as<std::uint16_t>());
        }
      }
    }

    if (trackNode["rating"])
    {
      builder.metadata().rating(trackNode["rating"].as<uint8_t>());
    }
  }

  void LibraryImporter::Impl::overlayCustomData(library::TrackBuilder& builder,
                                                YAML::Node const& trackNode,
                                                std::deque<std::string>& trackStrings) const
  {
    auto const keepAlive = [&](YAML::Node const& node) -> std::string_view
    {
      if (!node)
      {
        return {};
      }

      return trackStrings.emplace_back(node.template as<std::string>());
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

  void LibraryImporter::Impl::overlayTechnicalProperties(library::TrackBuilder& builder,
                                                         YAML::Node const& trackNode) const
  {
    using U32Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint32_t);
    using U16Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint16_t);
    using U8Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint8_t);

    struct Dispatch
    {
      rt::TrackField field;
      U32Setter u32Setter;
      U16Setter u16Setter;
      U8Setter u8Setter;
    };

    constexpr auto kPropertyDispatch = std::to_array<Dispatch>({
      {.field = rt::TrackField::Duration, .u32Setter = [](auto& prop, auto value) { prop.durationMs(value); }},
      {.field = rt::TrackField::Bitrate, .u32Setter = [](auto& prop, auto value) { prop.bitrate(value); }},
      {.field = rt::TrackField::SampleRate, .u32Setter = [](auto& prop, auto value) { prop.sampleRate(value); }},
      {.field = rt::TrackField::Codec, .u16Setter = [](auto& prop, auto value) { prop.codecId(value); }},
      {.field = rt::TrackField::Channels, .u8Setter = [](auto& prop, auto value) { prop.channels(value); }},
      {.field = rt::TrackField::BitDepth, .u8Setter = [](auto& prop, auto value) { prop.bitDepth(value); }},
    });

    for (auto const& map : kPropertyDispatch)
    {
      auto const key = rt::trackFieldId(map.field);

      if (auto node = trackNode[key.data()])
      {
        if (map.u32Setter != nullptr)
        {
          map.u32Setter(builder.property(), node.template as<std::uint32_t>());
        }
        else if (map.u16Setter != nullptr)
        {
          map.u16Setter(builder.property(), node.template as<std::uint16_t>());
        }
        else if (map.u8Setter != nullptr)
        {
          map.u8Setter(builder.property(), node.template as<std::uint8_t>());
        }
      }
    }

    if (trackNode["fileSize"])
    {
      builder.property().fileSize(trackNode["fileSize"].as<uint64_t>());
    }

    if (trackNode["mtime"])
    {
      builder.property().mtime(trackNode["mtime"].as<uint64_t>());
    }
  }

  void LibraryImporter::Impl::importLists(std::vector<ValidatedList> const& lists,
                                          lmdb::WriteTransaction& txn,
                                          std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                                          ImportMode strategy)
  {
    std::ignore = strategy;
    auto listWriter = ml.lists().writer(txn);
    auto manifestReader = ml.manifest().reader(txn);

    // Track ID mapping for this import session
    auto yamlListIdToNewListId = std::unordered_map<std::uint32_t, ListId>();
    yamlListIdToNewListId.reserve(lists.size());

    // 1. Create lists (initial pass without parent mapping)
    for (auto const& importedList : lists)
    {
      auto builder = library::ListBuilder::createNew().name(importedList.name).description(importedList.description);

      if (importedList.isSmart)
      {
        builder.filter(importedList.filter);
      }
      else
      {
        // Remap tracks using either YAML ID or URI
        for (auto const yamlTrackId : importedList.yamlTrackIds)
        {
          if (auto const it = yamlTrackIdToInternalId.find(yamlTrackId); it != yamlTrackIdToInternalId.end())
          {
            builder.tracks().add(it->second);
          }
        }

        for (auto const& uri : importedList.trackUris)
        {
          if (auto const optEntry = manifestReader.get(uri))
          {
            builder.tracks().add(optEntry->trackId);
          }
        }
      }

      // TODO: Implement "Real Merge" for lists (Phase 3/4)
      // For now, we always create new list records in this pass.
      auto [newListId, view] = listWriter.create(builder.serialize());
      std::ignore = view;
      yamlListIdToNewListId[importedList.yamlId] = newListId;
    }

    // 2. Resolve parent hierarchy and update
    for (auto const& importedList : lists)
    {
      if (importedList.yamlParentId == 0)
      {
        continue;
      }

      auto const parentIt = yamlListIdToNewListId.find(importedList.yamlParentId);

      if (parentIt == yamlListIdToNewListId.end())
      {
        // This should have been caught in validation
        continue;
      }

      auto const childId = yamlListIdToNewListId.at(importedList.yamlId);
      auto const optListView = listWriter.get(childId);

      if (!optListView)
      {
        continue;
      }

      auto builder = library::ListBuilder::fromView(*optListView).parentId(parentIt->second);
      listWriter.update(childId, builder.serialize());
    }
  }
} // namespace ao::rt
