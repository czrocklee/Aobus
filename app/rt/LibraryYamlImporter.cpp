// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/LibraryYamlImporter.h>

#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Base64.h>
#include <ao/rt/LibraryYamlExporter.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/yaml/Utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
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
      ryml::ConstNodeRef node;
    };

    struct ValidatedList final
    {
      std::uint32_t yamlId = 0;
      std::uint32_t yamlParentId = 0;
      std::string_view name;
      std::string_view description;
      std::string_view filter;
      std::vector<std::uint32_t> yamlTrackIds;
      std::vector<std::string> trackUris;
      bool isSmart = false;
    };

    struct ValidatedImport final
    {
      std::uint32_t version = 0;
      ExportMode payloadMode = ExportMode::Full;
      std::optional<std::string_view> optLibraryId;
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

  struct LibraryYamlImporter::Impl final
  {
    explicit Impl(library::MusicLibrary& ml)
      : ml{ml}
    {
    }

    Result<> importFromYaml(std::filesystem::path const& path, ImportMode mode);

    Result<ValidatedImport> validate(ryml::ConstNodeRef const& root) const;
    Result<> validateTracks(ryml::ConstNodeRef const& tracks, ValidatedImport& validated) const;
    Result<> validateLists(ryml::ConstNodeRef const& lists, ValidatedImport& validated) const;

    void importTracks(std::vector<ValidatedTrack> const& tracks,
                      lmdb::WriteTransaction& txn,
                      std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                      ImportMode strategy,
                      ExportMode payloadMode);
    void loadTrackBaseline(std::string_view uriStr,
                           std::optional<TrackId> const& optExistingTrackId,
                           ExportMode payloadMode,
                           std::optional<library::TrackBuilder>& optBuilder,
                           std::unique_ptr<tag::TagFile>& keepAliveTagFile,
                           library::TrackStore::Writer& trackWriter);

    void importLists(std::vector<ValidatedList> const& lists,
                     lmdb::WriteTransaction& txn,
                     std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                     ImportMode strategy);

    void overlayMetadata(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;
    void overlayCustomData(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;
    void overlayTechnicalProperties(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;

    void loadFileBaseline(std::string_view uriStr,
                          ExportMode payloadMode,
                          std::optional<library::TrackBuilder>& optBuilder,
                          std::unique_ptr<tag::TagFile>& keepAliveTagFile) const;

    library::MusicLibrary& ml;
  };

  LibraryYamlImporter::LibraryYamlImporter(library::MusicLibrary& ml)
    : _impl{std::make_unique<Impl>(ml)}
  {
  }

  LibraryYamlImporter::~LibraryYamlImporter() = default;

  Result<> LibraryYamlImporter::importFromYaml(std::filesystem::path const& path, ImportMode mode)
  {
    return _impl->importFromYaml(path, mode);
  }

  Result<> LibraryYamlImporter::Impl::importFromYaml(std::filesystem::path const& path, ImportMode mode)
  {
    auto buffer = std::vector<char>{};
    auto tree = ryml::Tree{};

    try
    {
      auto const fileName = path.string();
      buffer = yaml::readFile(path);
      tree = ryml::Tree{yaml::callbacks(fileName.c_str())};
      ryml::parse_in_place(yaml::toSubstr(buffer), &tree);
      tree.resolve();
    }
    catch (std::exception const& e)
    {
      return makeError(Error::Code::IoError, std::format("Failed to read '{}': {}", path.string(), e.what()));
    }

    auto const validationResult = validate(tree.rootref());

    if (!validationResult)
    {
      return std::unexpected{validationResult.error()};
    }

    auto const& validated = *validationResult;
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

    txn.commit();

    if (mode == ImportMode::Restore && validated.optLibraryId)
    {
      auto header = ml.metaHeader();
      header.libraryId = parseUuid(*validated.optLibraryId);
      ml.updateMetaHeader(header);
    }

    return {};
  }

  Result<ValidatedImport> LibraryYamlImporter::Impl::validate(ryml::ConstNodeRef const& root) const
  {
    auto validated = ValidatedImport{};

    auto const versionNode = yaml::findChild(root, "version");

    if (!versionNode.readable())
    {
      return makeError(Error::Code::FormatRejected, "Missing 'version' field in YAML");
    }

    validated.version = yaml::asInt<uint32_t>(versionNode);

    if (validated.version != 1)
    {
      return makeError(Error::Code::FormatRejected, std::format("Unsupported YAML version {}", validated.version));
    }

    if (auto const exportModeNode = yaml::findChild(root, "export_mode"); exportModeNode.readable())
    {
      validated.payloadMode = parseExportMode(yaml::scalarView(exportModeNode));
    }
    else
    {
      validated.payloadMode = ExportMode::Full;
    }

    if (auto const libraryIdNode = yaml::findChild(root, "libraryId"); libraryIdNode.readable())
    {
      validated.optLibraryId = yaml::scalarView(libraryIdNode);
    }

    auto const library = yaml::findChild(root, "library");

    if (!library.readable())
    {
      return makeError(Error::Code::FormatRejected, "Missing 'library' section in YAML");
    }

    if (validated.payloadMode != ExportMode::ListOnly)
    {
      if (auto const tracks = yaml::findChild(library, "tracks"); tracks.readable())
      {
        if (auto const result = validateTracks(tracks, validated); !result)
        {
          return std::unexpected{result.error()};
        }
      }
    }

    if (auto const lists = yaml::findChild(library, "lists"); lists.readable())
    {
      if (auto const result = validateLists(lists, validated); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return validated;
  }

  Result<> LibraryYamlImporter::Impl::validateTracks(ryml::ConstNodeRef const& tracks, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (auto const& trackNode : tracks.children())
    {
      auto track = ValidatedTrack{};
      auto const uriNode = yaml::findChild(trackNode, "uri");

      if (!uriNode.readable())
      {
        return makeError(Error::Code::FormatRejected, "Track record missing required 'uri' field");
      }

      auto rawUriStr = std::string{yaml::scalarView(uriNode)};
      std::ranges::replace(rawUriStr, '\\', '/');
      track.uri = std::filesystem::path{rawUriStr}.lexically_normal().generic_string();

      if (track.uri.empty())
      {
        return makeError(Error::Code::FormatRejected, "Track record has empty 'uri'");
      }

      if (auto const idNode = yaml::findChild(trackNode, "id"); idNode.readable())
      {
        track.yamlId = yaml::asInt<uint32_t>(idNode);

        if (track.yamlId != 0)
        {
          if (seenYamlIds.contains(track.yamlId))
          {
            return makeError(
              Error::Code::FormatRejected, std::format("Duplicate track YAML id {} in payload", track.yamlId));
          }

          seenYamlIds.insert(track.yamlId);
        }
      }

      track.node = trackNode;
      validated.tracks.push_back(std::move(track));
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::validateLists(ryml::ConstNodeRef const& lists, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (auto const& listNode : lists.children())
    {
      auto list = ValidatedList{};
      auto const idNode = yaml::findChild(listNode, "id");

      if (!idNode.readable())
      {
        return makeError(Error::Code::FormatRejected, "List record missing required 'id' field");
      }

      list.yamlId = yaml::asInt<uint32_t>(idNode);

      if (list.yamlId == 0)
      {
        return makeError(Error::Code::FormatRejected, "List id 0 is reserved for the root");
      }

      if (seenYamlIds.contains(list.yamlId))
      {
        return makeError(Error::Code::FormatRejected, std::format("Duplicate list YAML id {} in payload", list.yamlId));
      }

      seenYamlIds.insert(list.yamlId);

      auto const nameNode = yaml::findChild(listNode, "name");

      if (!nameNode.readable())
      {
        return makeError(Error::Code::FormatRejected, "List record missing required 'name' field");
      }

      list.name = yaml::scalarView(nameNode);

      if (auto const parentIdNode = yaml::findChild(listNode, "parentId"); parentIdNode.readable())
      {
        list.yamlParentId = yaml::asInt<uint32_t>(parentIdNode);
      }

      if (auto const descriptionNode = yaml::findChild(listNode, "description"); descriptionNode.readable())
      {
        list.description = yaml::scalarView(descriptionNode);
      }

      if (auto const filterNode = yaml::findChild(listNode, "filter"); filterNode.readable())
      {
        list.isSmart = true;
        list.filter = yaml::scalarView(filterNode);
      }
      else if (auto const tracksNode = yaml::findChild(listNode, "tracks"); tracksNode.readable())
      {
        for (auto const& trackRef : tracksNode.children())
        {
          if (trackRef.is_val())
          {
            list.yamlTrackIds.push_back(yaml::asInt<uint32_t>(trackRef));
          }
          else if (trackRef.is_map())
          {
            if (auto const trackIdNode = yaml::findChild(trackRef, "id"); trackIdNode.readable())
            {
              list.yamlTrackIds.push_back(yaml::asInt<uint32_t>(trackIdNode));
            }
            else if (auto const uriNode = yaml::findChild(trackRef, "uri"); uriNode.readable())
            {
              auto rawUriStr = std::string{yaml::scalarView(uriNode)};
              std::ranges::replace(rawUriStr, '\\', '/');
              list.trackUris.push_back(std::filesystem::path{rawUriStr}.lexically_normal().generic_string());
            }
          }
        }
      }

      validated.lists.push_back(std::move(list));
    }

    return {};
  }

  void LibraryYamlImporter::Impl::importTracks(std::vector<ValidatedTrack> const& tracks,
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
      std::string_view const& uriStr = validatedTrack.uri;

      auto optExistingTrackId = std::optional<TrackId>{};

      if (strategy == ImportMode::Merge)
      {
        if (auto const optManifestView = manifestReader.get(uriStr))
        {
          optExistingTrackId = optManifestView->trackId();
        }
      }

      auto optBuilder = std::optional<library::TrackBuilder>{};
      auto keepAliveTagFile = std::unique_ptr<tag::TagFile>{};

      loadTrackBaseline(uriStr, optExistingTrackId, payloadMode, optBuilder, keepAliveTagFile, trackWriter);

      auto builder = optBuilder ? *optBuilder : library::TrackBuilder::createNew();

      if (!optBuilder)
      {
        builder.property().uri(uriStr);
      }

      overlayMetadata(builder, trackNode);
      overlayCustomData(builder, trackNode);
      overlayTechnicalProperties(builder, trackNode);

      auto coverArtBinary = std::vector<std::byte>{};

      if (auto const coverArtNode = yaml::findChild(trackNode, "coverArtBase64"); coverArtNode.readable())
      {
        auto const b64 = yaml::scalarView(coverArtNode);
        coverArtBinary = utility::base64Decode(b64);

        if (!coverArtBinary.empty())
        {
          builder.metadata().coverArtData(coverArtBinary);
        }
      }

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

      auto manifestBuilder = library::FileManifestBuilder::createNew();
      manifestBuilder.trackId(targetTrackId);

      if (auto const optManifestView = manifestReader.get(uriStr))
      {
        manifestBuilder.fileSize(optManifestView->fileSize());
        manifestBuilder.mtime(optManifestView->mtime());
      }
      else if (auto const fullPath = ml.rootPath() / uriStr; std::filesystem::exists(fullPath))
      {
        manifestBuilder.fileSize(std::filesystem::file_size(fullPath));
        manifestBuilder.mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::filesystem::last_write_time(fullPath).time_since_epoch())
                                .count());
      }

      if (auto fileSizeNode = yaml::findChild(trackNode, "fileSize"); fileSizeNode.readable())
      {
        manifestBuilder.fileSize(yaml::asInt<uint64_t>(fileSizeNode));
      }

      if (auto mtimeNode = yaml::findChild(trackNode, "mtime"); mtimeNode.readable())
      {
        manifestBuilder.mtime(yaml::asInt<uint64_t>(mtimeNode));
      }

      manifestWriter.put(uriStr, manifestBuilder.serialize());

      if (validatedTrack.yamlId != 0)
      {
        yamlTrackIdToInternalId[validatedTrack.yamlId] = targetTrackId;
      }
    }
  }

  void LibraryYamlImporter::Impl::loadTrackBaseline(std::string_view uriStr,
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

  void LibraryYamlImporter::Impl::loadFileBaseline(std::string_view uriStr,
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
            if (optBuilder->metadata().coverArtId() == 0)
            {
              optBuilder->metadata().coverArtData(fileBuilder.metadata().coverArtData());
            }
          }
        }

        optBuilder->property().uri(uriStr);
      }
    }
  }

  void LibraryYamlImporter::Impl::overlayMetadata(library::TrackBuilder& builder,
                                                  ryml::ConstNodeRef const& trackNode) const
  {
    using StringSetter = void (*)(library::TrackBuilder::MetadataBuilder&, std::string_view);
    using NumberSetter = void (*)(library::TrackBuilder::MetadataBuilder&, std::uint16_t);

    struct Dispatch
    {
      rt::TrackField field;
      StringSetter stringSetter = nullptr;
      NumberSetter numberSetter = nullptr;
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

      if (auto node = yaml::findChild(trackNode, key); node.readable())
      {
        if (map.stringSetter != nullptr)
        {
          map.stringSetter(builder.metadata(), yaml::scalarView(node));
        }
        else if (map.numberSetter != nullptr)
        {
          map.numberSetter(builder.metadata(), yaml::asInt<std::uint16_t>(node));
        }
      }
    }

    if (auto ratingNode = yaml::findChild(trackNode, "rating"); ratingNode.readable())
    {
      builder.metadata().rating(yaml::asInt<uint8_t>(ratingNode));
    }
  }

  void LibraryYamlImporter::Impl::overlayCustomData(library::TrackBuilder& builder,
                                                    ryml::ConstNodeRef const& trackNode) const
  {
    if (auto tagsNode = yaml::findChild(trackNode, "tags"); tagsNode.readable())
    {
      builder.tags().clear();

      for (auto const& tag : tagsNode.children())
      {
        builder.tags().add(yaml::scalarView(tag));
      }
    }

    if (auto customNode = yaml::findChild(trackNode, "custom"); customNode.readable())
    {
      builder.custom().clear();

      for (auto const& it : customNode.children())
      {
        builder.custom().add(yaml::keyView(it), yaml::scalarView(it));
      }
    }
  }

  void LibraryYamlImporter::Impl::overlayTechnicalProperties(library::TrackBuilder& builder,
                                                             ryml::ConstNodeRef const& trackNode) const
  {
    using U32Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint32_t);
    using U16Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint16_t);
    using U8Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint8_t);

    struct Dispatch
    {
      rt::TrackField field;
      U32Setter u32Setter = nullptr;
      U16Setter u16Setter = nullptr;
      U8Setter u8Setter = nullptr;
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

      if (auto node = yaml::findChild(trackNode, key); node.readable())
      {
        if (map.u32Setter != nullptr)
        {
          map.u32Setter(builder.property(), yaml::asInt<std::uint32_t>(node));
        }
        else if (map.u16Setter != nullptr)
        {
          map.u16Setter(builder.property(), yaml::asInt<std::uint16_t>(node));
        }
        else if (map.u8Setter != nullptr)
        {
          map.u8Setter(builder.property(), yaml::asInt<std::uint8_t>(node));
        }
      }
    }
  }

  void LibraryYamlImporter::Impl::importLists(std::vector<ValidatedList> const& lists,
                                              lmdb::WriteTransaction& txn,
                                              std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                                              ImportMode strategy)
  {
    std::ignore = strategy;
    auto listWriter = ml.lists().writer(txn);
    auto manifestReader = ml.manifest().reader(txn);

    auto yamlListIdToNewListId = std::unordered_map<std::uint32_t, ListId>{};
    yamlListIdToNewListId.reserve(lists.size());

    for (auto const& importedList : lists)
    {
      auto builder = library::ListBuilder::createNew().name(importedList.name).description(importedList.description);

      if (importedList.isSmart)
      {
        builder.filter(importedList.filter);
      }
      else
      {
        for (auto const yamlTrackId : importedList.yamlTrackIds)
        {
          if (auto const it = yamlTrackIdToInternalId.find(yamlTrackId); it != yamlTrackIdToInternalId.end())
          {
            builder.tracks().add(it->second);
          }
        }

        for (auto const& uri : importedList.trackUris)
        {
          if (auto const optManifestView = manifestReader.get(uri))
          {
            builder.tracks().add(optManifestView->trackId());
          }
        }
      }

      auto [newListId, view] = listWriter.create(builder.serialize());
      std::ignore = view;
      yamlListIdToNewListId[importedList.yamlId] = newListId;
    }

    for (auto const& importedList : lists)
    {
      if (importedList.yamlParentId == 0)
      {
        continue;
      }

      auto const parentIt = yamlListIdToNewListId.find(importedList.yamlParentId);

      if (parentIt == yamlListIdToNewListId.end())
      {
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
