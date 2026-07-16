// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "MediaTrack.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/utility/Base64.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

namespace ao::rt
{
  namespace
  {
    using MetadataStringGetter = std::string_view (*)(library::TrackView const&, library::DictionaryStore const&);
    using MetadataStringBaseGetter = std::string_view (*)(library::TrackBuilder::MetadataBuilder const&);
    using MetadataNumberGetter = std::uint16_t (*)(library::TrackView const&);
    using MetadataNumberBaseGetter = std::uint16_t (*)(library::TrackBuilder::MetadataBuilder const&);

    struct MetadataDispatch final
    {
      TrackField field;
      MetadataStringGetter stringGetter = nullptr;
      MetadataStringBaseGetter baseStringGetter = nullptr;
      MetadataNumberGetter numberGetter = nullptr;
      MetadataNumberBaseGetter baseNumberGetter = nullptr;
    };

    constexpr auto kMetadataDispatch = std::to_array<MetadataDispatch>({
      {.field = TrackField::Title,
       .stringGetter = [](auto const& view, auto&) { return view.metadata().title(); },
       .baseStringGetter = [](auto const& base) { return base.title(); }},
      {.field = TrackField::Artist,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().artistId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.artist(); }},
      {.field = TrackField::Album,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().albumId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.album(); }},
      {.field = TrackField::AlbumArtist,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().albumArtistId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.albumArtist(); }},
      {.field = TrackField::Composer,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().composerId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.composer(); }},
      {.field = TrackField::Conductor,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().conductorId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.conductor(); }},
      {.field = TrackField::Ensemble,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().ensembleId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.ensemble(); }},
      {.field = TrackField::Genre,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().genreId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.genre(); }},
      {.field = TrackField::Work,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().workId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.work(); }},
      {.field = TrackField::Movement,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().movementId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.movement(); }},
      {.field = TrackField::Soloist,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().soloistId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.soloist(); }},
      {.field = TrackField::Year,
       .numberGetter = [](auto const& view) { return view.metadata().year(); },
       .baseNumberGetter = [](auto const& base) { return base.year(); }},
      {.field = TrackField::TrackNumber,
       .numberGetter = [](auto const& view) { return view.metadata().trackNumber(); },
       .baseNumberGetter = [](auto const& base) { return base.trackNumber(); }},
      {.field = TrackField::TrackTotal,
       .numberGetter = [](auto const& view) { return view.metadata().trackTotal(); },
       .baseNumberGetter = [](auto const& base) { return base.trackTotal(); }},
      {.field = TrackField::DiscNumber,
       .numberGetter = [](auto const& view) { return view.metadata().discNumber(); },
       .baseNumberGetter = [](auto const& base) { return base.discNumber(); }},
      {.field = TrackField::DiscTotal,
       .numberGetter = [](auto const& view) { return view.metadata().discTotal(); },
       .baseNumberGetter = [](auto const& base) { return base.discTotal(); }},
      {.field = TrackField::MovementNumber,
       .numberGetter = [](auto const& view) { return view.classical().movementNumber(); },
       .baseNumberGetter = [](auto const& base) { return base.movementNumber(); }},
      {.field = TrackField::MovementTotal,
       .numberGetter = [](auto const& view) { return view.classical().movementTotal(); },
       .baseNumberGetter = [](auto const& base) { return base.movementTotal(); }},
    });

    void appendString(ryml::NodeRef& node, std::string_view key, std::string_view value)
    {
      auto child = node.append_child();
      yaml::setKey(child, key);
      yaml::setValue(child, value);
    }

    void emitTrackMetadata(ryml::NodeRef& node,
                           library::TrackView const& view,
                           library::DictionaryStore const& dictionary,
                           std::optional<library::TrackBuilder> const& optBaseline)
    {
      auto const optBaselineMetadata = optBaseline ? std::optional{optBaseline->metadata()} : std::nullopt;
      auto const hasBaseline = optBaselineMetadata.has_value();

      for (auto const& map : kMetadataDispatch)
      {
        if (auto const key = trackFieldId(map.field); map.stringGetter != nullptr)
        {
          auto const current = map.stringGetter(view, dictionary);
          bool const shouldEmit =
            hasBaseline ? (current != map.baseStringGetter(*optBaselineMetadata)) : !current.empty();

          if (shouldEmit)
          {
            appendString(node, key, current);
          }
        }
        else if (map.numberGetter != nullptr)
        {
          auto const current = map.numberGetter(view);
          bool const shouldEmit =
            hasBaseline ? (current != map.baseNumberGetter(*optBaselineMetadata)) : (current != 0);

          if (shouldEmit)
          {
            node.append_child() << ryml::key(key) << current;
          }
        }
      }

      if (auto const custom = view.customMetadata(); !custom.empty())
      {
        auto customNode = node.append_child();
        yaml::setKey(customNode, "custom");
        customNode |= ryml::MAP;

        for (auto const& [dictionaryId, value] : custom)
        {
          auto child = customNode.append_child();
          yaml::setKey(child, dictionary.get(dictionaryId));
          yaml::setValue(child, value);
        }
      }
    }

    using PropertyU64Getter = std::uint64_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU32Getter = std::uint32_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU16Getter = std::uint16_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU8Getter = std::uint8_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyStringGetter = std::string_view (*)(library::TrackView::PropertyProxy const&);

    struct PropertyDispatch final
    {
      TrackField field;
      PropertyU64Getter u64Get = nullptr;
      PropertyU32Getter u32Get = nullptr;
      PropertyU16Getter u16Get = nullptr;
      PropertyU8Getter u8Get = nullptr;
      PropertyStringGetter stringGet = nullptr;
    };

    constexpr auto kPropertyDispatch = std::to_array<PropertyDispatch>({
      {.field = TrackField::Duration,
       .u32Get = [](auto const& prop) { return static_cast<std::uint32_t>(prop.duration().count()); }},
      {.field = TrackField::Bitrate, .u32Get = [](auto const& prop) { return prop.bitrate().raw(); }},
      {.field = TrackField::SampleRate, .u32Get = [](auto const& prop) { return prop.sampleRate().raw(); }},
      {.field = TrackField::Codec,
       .stringGet = [](auto const& prop)
       { return prop.codec() == AudioCodec::Unknown ? std::string_view{"UNKNOWN"} : audioCodecName(prop.codec()); }},
      {.field = TrackField::Channels, .u8Get = [](auto const& prop) { return prop.channels().raw(); }},
      {.field = TrackField::BitDepth, .u8Get = [](auto const& prop) { return prop.bitDepth().raw(); }},
    });

    Result<> emitTrackProperties(ryml::NodeRef& node,
                                 library::TrackView::PropertyProxy const& property,
                                 library::FileManifestStore::Reader const& manifestReader)
    {
      for (auto const& map : kPropertyDispatch)
      {
        if (auto const key = trackFieldId(map.field); map.u64Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u64Get(property);
        }
        else if (map.u32Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u32Get(property);
        }
        else if (map.u16Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u16Get(property);
        }
        else if (map.u8Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u8Get(property);
        }
        else if (map.stringGet != nullptr)
        {
          node.append_child() << ryml::key(key) << map.stringGet(property);
        }
      }

      std::uint64_t fileSize = 0;
      std::uint64_t mtime = 0;

      auto const manifestResult = manifestReader.get(property.uri());

      if (manifestResult)
      {
        fileSize = manifestResult->fileSize();
        mtime = manifestResult->mtime();
      }
      else if (manifestResult.error().code != Error::Code::NotFound)
      {
        return std::unexpected{manifestResult.error()};
      }

      node.append_child() << ryml::key("fileSize") << fileSize;
      node.append_child() << ryml::key("mtime") << mtime;
      return {};
    }

    bool matchesCoverBaseline(library::CoverArt const& cover,
                              library::TrackBuilder::CoverArtBuilder::PendingCoverArt const& baseline,
                              library::ResourceStore::Reader const& resReader)
    {
      auto const optDbData = resReader.get(cover.resourceId);

      auto const* baselineResourceId = std::get_if<ResourceId>(&baseline.source);
      auto const* baselineData = std::get_if<std::span<std::byte const>>(&baseline.source);

      return baseline.type == cover.type &&
             (baselineResourceId == nullptr || *baselineResourceId == cover.resourceId) &&
             (baselineData == nullptr || (optDbData && std::ranges::equal(*optDbData, *baselineData)));
    }

    bool shouldExportCovers(library::TrackView const& view,
                            std::optional<library::TrackBuilder> const& optBaseline,
                            ExportMode mode,
                            library::ResourceStore::Reader const& resReader)
    {
      if (mode == ExportMode::Metadata || mode == ExportMode::Full)
      {
        return true;
      }

      if (mode != ExportMode::Delta)
      {
        return false;
      }

      if (!optBaseline)
      {
        return true;
      }

      auto const covers = view.coverArt();
      auto const coverCount = covers.count();
      auto const& baseCovers = optBaseline->coverArt().entries();

      if (coverCount != baseCovers.size())
      {
        return true;
      }

      for (std::uint16_t i = 0; i < coverCount; ++i)
      {
        auto const cover = covers.at(i);

        if (auto const& baseline = baseCovers[i]; !matchesCoverBaseline(cover, baseline, resReader))
        {
          return true;
        }
      }

      return false;
    }

    Result<> emitSingleCover(ryml::NodeRef& coverNode,
                             ResourceId resId,
                             std::uint8_t typeValue,
                             std::unordered_map<ResourceId, std::string>& exportedCovers,
                             library::ResourceStore::Reader const& resReader)
    {
      coverNode.append_child() << ryml::key("type") << static_cast<std::uint32_t>(typeValue);

      if (auto const it = exportedCovers.find(resId); it != exportedCovers.end())
      {
        auto dataNode = coverNode.append_child();
        dataNode << ryml::key("data");
        dataNode.set_val_ref(yaml::copyToArena(dataNode, it->second));
        return {};
      }

      auto const optDbData = resReader.get(resId);

      if (!optDbData || optDbData->empty())
      {
        return makeError(Error::Code::CorruptData, std::format("Cover resource {} is missing or empty", resId.raw()));
      }

      auto const b64 = utility::base64Encode(*optDbData);
      auto const anchorName = "cover_" + std::to_string(resId.raw());

      auto dataNode = coverNode.append_child();
      dataNode << ryml::key("data") << b64;
      dataNode.set_val_anchor(yaml::copyToArena(dataNode, anchorName));
      exportedCovers[resId] = anchorName;
      return {};
    }

    Result<> emitTrackCover(ryml::NodeRef& node,
                            library::ReadTransaction const& transaction,
                            library::TrackView const& view,
                            std::optional<library::TrackBuilder> const& optBaseline,
                            ExportMode mode,
                            std::unordered_map<ResourceId, std::string>& exportedCovers,
                            library::ResourceStore const& resources)
    {
      auto const resReader = resources.reader(transaction);

      if (!shouldExportCovers(view, optBaseline, mode, resReader))
      {
        return {};
      }

      auto coversNode = node.append_child();
      yaml::setKey(coversNode, "covers");
      coversNode |= ryml::SEQ;

      auto const covers = view.coverArt();

      for (std::uint16_t i = 0; i < covers.count(); ++i)
      {
        auto const cover = covers.at(i);
        auto coverNode = coversNode.append_child();
        coverNode |= ryml::MAP;

        if (auto result = emitSingleCover(
              coverNode, cover.resourceId, static_cast<std::uint8_t>(cover.type), exportedCovers, resReader);
            !result)
        {
          return result;
        }
      }

      return {};
    }

    void emitTrackCommon(ryml::NodeRef& node,
                         library::TrackView::TagProxy const& tags,
                         library::DictionaryStore const& dictionary)
    {
      if (tags.count() != 0)
      {
        auto tagsNode = node.append_child();
        yaml::setKey(tagsNode, "tags");
        tagsNode |= ryml::SEQ;

        for (auto const tagId : tags)
        {
          yaml::setValue(tagsNode.append_child(), dictionary.get(tagId));
        }
      }
    }

    Result<> emitList(ryml::NodeRef& listsNode,
                      ListId const listId,
                      library::ListView const& listView,
                      ExportMode const mode,
                      library::TrackStore::Reader const& trackReader)
    {
      auto listNode = listsNode.append_child();
      listNode |= ryml::MAP;

      listNode.append_child() << ryml::key("id") << listId.raw();
      listNode.append_child() << ryml::key("parentId") << listView.parentId().raw();
      appendString(listNode, "name", listView.name());

      if (!listView.description().empty())
      {
        appendString(listNode, "description", listView.description());
      }

      if (listView.isSmart())
      {
        appendString(listNode, "filter", listView.filter());
        return {};
      }

      auto const tracks = listView.tracks();

      if (tracks.empty())
      {
        return {};
      }

      auto tracksNode = listNode.append_child();
      yaml::setKey(tracksNode, "tracks");
      tracksNode |= ryml::SEQ;

      for (auto const trackId : tracks)
      {
        if (mode != ExportMode::ListOnly)
        {
          tracksNode.append_child() << trackId.raw();
          continue;
        }

        auto const optTrackView = trackReader.get(trackId);

        if (!optTrackView)
        {
          continue;
        }

        auto uri = library::LibraryUri::parse(optTrackView->property().uri());

        if (!uri || uri->value() != optTrackView->property().uri())
        {
          return makeError(
            Error::Code::CorruptData, std::format("Track {} contains an invalid library URI", trackId.raw()));
        }

        auto refNode = tracksNode.append_child();
        refNode |= ryml::MAP;
        appendString(refNode, "uri", uri->value());
      }

      return {};
    }
  } // namespace

  struct LibraryYamlExporter::Impl final
  {
    explicit Impl(library::MusicLibrary const& ml)
      : ml{ml}
    {
    }

    Result<> exportToYaml(std::filesystem::path const& path, ExportMode mode) const;
    Result<> exportTracks(ryml::NodeRef& node, library::ReadTransaction const& transaction, ExportMode mode) const;
    Result<> exportTrack(ryml::NodeRef& node,
                         library::ReadTransaction const& transaction,
                         TrackId id,
                         library::TrackView const& view,
                         ExportMode mode,
                         std::unordered_map<ResourceId, std::string>& exportedCovers,
                         library::ResourceStore const& resources,
                         library::DictionaryStore const& dictionary,
                         library::FileManifestStore::Reader const& manifestReader) const;
    Result<> exportLists(ryml::NodeRef& node, library::ReadTransaction const& transaction, ExportMode mode) const;

    library::MusicLibrary const& ml;
  };

  LibraryYamlExporter::LibraryYamlExporter(library::MusicLibrary const& ml)
    : _implPtr{std::make_unique<Impl>(ml)}
  {
  }

  LibraryYamlExporter::~LibraryYamlExporter() = default;

  Result<> LibraryYamlExporter::exportToYaml(std::filesystem::path const& path, ExportMode mode)
  {
    return _implPtr->exportToYaml(path, mode);
  }

  Result<> LibraryYamlExporter::Impl::exportToYaml(std::filesystem::path const& path, ExportMode mode) const
  {
    auto tree = ryml::Tree{};
    auto root = tree.rootref();
    root |= ryml::MAP;

    auto const transaction = ml.readTransaction();
    auto const header = ml.metadata().load(transaction);

    if (!header)
    {
      return std::unexpected{header.error()};
    }

    root.append_child() << ryml::key("version") << 2;
    appendString(root, "libraryId", utility::formatUuid(header->libraryId));
    appendString(root, "export_mode", exportModeName(mode));

    auto library = root.append_child();
    yaml::setKey(library, "library");
    library |= ryml::MAP;

    if (mode != ExportMode::ListOnly)
    {
      if (auto result = exportTracks(library, transaction, mode); !result)
      {
        return result;
      }
    }

    if (auto result = exportLists(library, transaction, mode); !result)
    {
      return result;
    }

    auto ofs = std::ofstream{path};

    if (!ofs)
    {
      return makeError(Error::Code::IoError, std::format("Failed to open '{}' for writing", path.string()));
    }

    std::string const yaml = ryml::emitrs_yaml<std::string>(tree);
    ofs.write(yaml.data(), static_cast<std::streamsize>(yaml.size()));

    if (!ofs.good())
    {
      return makeError(Error::Code::IoError, std::format("File write error while writing '{}'", path.string()));
    }

    return {};
  }

  Result<> LibraryYamlExporter::Impl::exportTracks(ryml::NodeRef& node,
                                                   library::ReadTransaction const& transaction,
                                                   ExportMode mode) const
  {
    auto const trackReader = ml.tracks().reader(transaction);
    auto const manifestReader = ml.manifest().reader(transaction);
    auto const& resources = ml.resources();
    auto const& dictionary = ml.dictionary();
    auto exportedCovers = std::unordered_map<ResourceId, std::string>{};

    auto tracksNode = node.append_child();
    yaml::setKey(tracksNode, "tracks");
    tracksNode |= ryml::SEQ;

    for (auto const& [trackId, view] : trackReader)
    {
      if (auto result = exportTrack(
            tracksNode, transaction, trackId, view, mode, exportedCovers, resources, dictionary, manifestReader);
          !result)
      {
        return result;
      }
    }

    return {};
  }

  Result<> LibraryYamlExporter::Impl::exportTrack(ryml::NodeRef& node,
                                                  library::ReadTransaction const& transaction,
                                                  TrackId id,
                                                  library::TrackView const& view,
                                                  ExportMode mode,
                                                  std::unordered_map<ResourceId, std::string>& exportedCovers,
                                                  library::ResourceStore const& resources,
                                                  library::DictionaryStore const& dictionary,
                                                  library::FileManifestStore::Reader const& manifestReader) const
  {
    auto trackNode = node.append_child();
    trackNode |= ryml::MAP;

    trackNode.append_child() << ryml::key("id") << id.raw();

    auto const property = view.property();
    auto uri = library::LibraryUri::parse(property.uri());

    if (!uri || uri->value() != property.uri())
    {
      return makeError(Error::Code::CorruptData, std::format("Track {} contains an invalid library URI", id.raw()));
    }

    appendString(trackNode, "uri", uri->value());

    auto optMediaTrack = std::optional<MediaTrack>{};
    auto optBaseline = std::optional<library::TrackBuilder>{};

    if (mode == ExportMode::Delta)
    {
      auto fileEc = std::error_code{};

      auto fullPathResult = uri->resolveUnder(ml.rootPath());

      if (!fullPathResult)
      {
        return std::unexpected{fullPathResult.error()};
      }

      if (auto const& fullPath = *fullPathResult; std::filesystem::exists(fullPath, fileEc) && !fileEc)
      {
        if (auto mediaTrackResult = readMediaTrack(fullPath); mediaTrackResult)
        {
          optMediaTrack.emplace(std::move(*mediaTrackResult));
          optBaseline = optMediaTrack->builder();
        }
      }
      else if (fileEc)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to inspect file '{}': {}", fullPath.string(), fileEc.message()));
      }
    }

    if (mode != ExportMode::ListOnly)
    {
      emitTrackMetadata(trackNode, view, dictionary, optBaseline);
    }

    if (mode == ExportMode::Full)
    {
      if (auto result = emitTrackProperties(trackNode, property, manifestReader); !result)
      {
        return result;
      }
    }

    if (auto result = emitTrackCover(trackNode, transaction, view, optBaseline, mode, exportedCovers, resources);
        !result)
    {
      return result;
    }

    emitTrackCommon(trackNode, view.tags(), dictionary);
    return {};
  }

  Result<> LibraryYamlExporter::Impl::exportLists(ryml::NodeRef& node,
                                                  library::ReadTransaction const& transaction,
                                                  ExportMode mode) const
  {
    auto listsNode = node.append_child();
    yaml::setKey(listsNode, "lists");
    listsNode |= ryml::SEQ;

    auto const listReader = ml.lists().reader(transaction);
    auto const trackReader = ml.tracks().reader(transaction);

    for (auto const& [listId, listView] : listReader)
    {
      if (auto result = emitList(listsNode, listId, listView, mode, trackReader); !result)
      {
        return result;
      }
    }

    return {};
  }
} // namespace ao::rt
