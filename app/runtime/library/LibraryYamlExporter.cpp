// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/Meta.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Base64.h>
#include <ao/yaml/Utils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kUuidByteCount = 16;
    constexpr std::size_t kHexCharsPerByte = 2;
    constexpr std::size_t kUuidTimeLowByteCount = 4;
    constexpr std::size_t kUuidTimeMidByteCount = 2;
    constexpr std::size_t kUuidTimeHighByteCount = 2;
    constexpr std::size_t kUuidClockSeqByteCount = 2;
    constexpr std::size_t kUuidNodeByteCount = 6;
    constexpr auto kUuidGroupByteCounts = std::to_array<std::size_t>({kUuidTimeLowByteCount,
                                                                      kUuidTimeMidByteCount,
                                                                      kUuidTimeHighByteCount,
                                                                      kUuidClockSeqByteCount,
                                                                      kUuidNodeByteCount});
    constexpr auto kUuidHyphenCount = kUuidGroupByteCounts.size() - 1;
    constexpr auto kUuidTextLength = (kUuidByteCount * kHexCharsPerByte) + kUuidHyphenCount;

    std::string formatUuid(std::array<std::byte, kUuidByteCount> const& id)
    {
      auto result = std::string{};
      result.reserve(kUuidTextLength);
      std::size_t byteIndex = 0;

      for (std::size_t groupIndex = 0; groupIndex < kUuidGroupByteCounts.size(); ++groupIndex)
      {
        if (groupIndex > 0)
        {
          result.push_back('-');
        }

        for (std::size_t groupByte = 0; groupByte < kUuidGroupByteCounts.at(groupIndex); ++groupByte)
        {
          result += std::format("{:02x}", static_cast<unsigned char>(id.at(byteIndex)));
          ++byteIndex;
        }
      }

      return result;
    }

    std::string_view modeToString(ExportMode mode)
    {
      switch (mode)
      {
        case ExportMode::Delta: return "delta";
        case ExportMode::Metadata: return "metadata";
        case ExportMode::Full: return "full";
        case ExportMode::ListOnly: return "listOnly";
      }

      return "unknown";
    }

    using MetadataStringGetter = std::string_view (*)(library::TrackView::MetadataProxy const&,
                                                      library::DictionaryStore&);
    using MetadataStringBaseGetter = std::string_view (*)(library::TrackBuilder::MetadataBuilder const&);
    using MetadataNumberGetter = std::uint16_t (*)(library::TrackView::MetadataProxy const&);
    using MetadataNumberBaseGetter = std::uint16_t (*)(library::TrackBuilder::MetadataBuilder const&);

    struct MetadataDispatch final
    {
      TrackField field;
      MetadataStringGetter strGet = nullptr;
      MetadataStringBaseGetter strBaseGet = nullptr;
      MetadataNumberGetter numGet = nullptr;
      MetadataNumberBaseGetter numBaseGet = nullptr;
    };

    constexpr auto kMetadataDispatch = std::to_array<MetadataDispatch>({
      {.field = TrackField::Title,
       .strGet = [](auto const& meta, auto&) { return meta.title(); },
       .strBaseGet = [](auto const& base) { return base.title(); }},
      {.field = TrackField::Artist,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.artistId() != kInvalidDictionaryId ? dict.get(meta.artistId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.artist(); }},
      {.field = TrackField::Album,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.albumId() != kInvalidDictionaryId ? dict.get(meta.albumId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.album(); }},
      {.field = TrackField::AlbumArtist,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.albumArtistId() != kInvalidDictionaryId ? dict.get(meta.albumArtistId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.albumArtist(); }},
      {.field = TrackField::Composer,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.composerId() != kInvalidDictionaryId ? dict.get(meta.composerId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.composer(); }},
      {.field = TrackField::Genre,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.genreId() != kInvalidDictionaryId ? dict.get(meta.genreId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.genre(); }},
      {.field = TrackField::Work,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.workId() != kInvalidDictionaryId ? dict.get(meta.workId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.work(); }},
      {.field = TrackField::Movement,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.movementId() != kInvalidDictionaryId ? dict.get(meta.movementId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.movement(); }},
      {.field = TrackField::Year,
       .numGet = [](auto const& meta) { return meta.year(); },
       .numBaseGet = [](auto const& base) { return base.year(); }},
      {.field = TrackField::TrackNumber,
       .numGet = [](auto const& meta) { return meta.trackNumber(); },
       .numBaseGet = [](auto const& base) { return base.trackNumber(); }},
      {.field = TrackField::TrackTotal,
       .numGet = [](auto const& meta) { return meta.trackTotal(); },
       .numBaseGet = [](auto const& base) { return base.trackTotal(); }},
      {.field = TrackField::DiscNumber,
       .numGet = [](auto const& meta) { return meta.discNumber(); },
       .numBaseGet = [](auto const& base) { return base.discNumber(); }},
      {.field = TrackField::DiscTotal,
       .numGet = [](auto const& meta) { return meta.discTotal(); },
       .numBaseGet = [](auto const& base) { return base.discTotal(); }},
      {.field = TrackField::MovementNumber,
       .numGet = [](auto const& meta) { return meta.movementNumber(); },
       .numBaseGet = [](auto const& base) { return base.movementNumber(); }},
      {.field = TrackField::MovementTotal,
       .numGet = [](auto const& meta) { return meta.movementTotal(); },
       .numBaseGet = [](auto const& base) { return base.movementTotal(); }},
    });

    void appendString(ryml::NodeRef& node, std::string_view key, std::string_view value)
    {
      auto child = node.append_child();
      yaml::setKey(child, key);
      yaml::setValue(child, value);
    }

    void emitTrackMetadata(ryml::NodeRef& node,
                           library::TrackView const& view,
                           library::DictionaryStore& dict,
                           std::optional<library::TrackBuilder> const& optBaseline)
    {
      auto const metadata = view.metadata();
      auto const optBaselineMeta = optBaseline ? std::optional{optBaseline->metadata()} : std::nullopt;
      auto const hasBaseline = static_cast<bool>(optBaselineMeta);

      for (auto const& map : kMetadataDispatch)
      {
        if (auto const key = trackFieldId(map.field); map.strGet != nullptr)
        {
          auto const current = map.strGet(metadata, dict);
          bool const shouldEmit = hasBaseline ? (current != map.strBaseGet(*optBaselineMeta)) : !current.empty();

          if (shouldEmit)
          {
            appendString(node, key, current);
          }
        }
        else if (map.numGet != nullptr)
        {
          auto const current = map.numGet(metadata);
          bool const shouldEmit = hasBaseline ? (current != map.numBaseGet(*optBaselineMeta)) : (current != 0);

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

        for (auto const& [dictId, value] : custom)
        {
          auto child = customNode.append_child();
          yaml::setKey(child, dict.get(dictId));
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
      {.field = TrackField::Codec, .stringGet = [](auto const& prop) { return library::audioCodecName(prop.codec()); }},
      {.field = TrackField::Channels, .u8Get = [](auto const& prop) { return prop.channels().raw(); }},
      {.field = TrackField::BitDepth, .u8Get = [](auto const& prop) { return prop.bitDepth().raw(); }},
    });

    void emitTrackProperties(ryml::NodeRef& node,
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

      if (auto const optManifestView = manifestReader.get(property.uri()); optManifestView)
      {
        fileSize = optManifestView->fileSize();
        mtime = optManifestView->mtime();
      }

      node.append_child() << ryml::key("fileSize") << fileSize;
      node.append_child() << ryml::key("mtime") << mtime;
    }

    void emitTrackCover(ryml::NodeRef& node,
                        lmdb::ReadTransaction const& txn,
                        library::TrackView const& view,
                        std::optional<library::TrackBuilder> const& optBaseline,
                        ExportMode mode,
                        std::unordered_map<ResourceId, std::string>& exportedCovers,
                        library::ResourceStore& resources)
    {
      auto const covers = view.coverArt();
      auto const coverCount = covers.count();
      bool shouldExportCovers = (mode == ExportMode::Metadata || mode == ExportMode::Full);
      auto const resReader = resources.reader(txn);

      if (mode == ExportMode::Delta)
      {
        if (!optBaseline)
        {
          shouldExportCovers = true;
        }
        else
        {
          auto const& baseCovers = optBaseline->coverArt().entries();
          shouldExportCovers = coverCount != baseCovers.size();

          for (std::uint16_t i = 0; !shouldExportCovers && i < coverCount; ++i)
          {
            auto const cover = covers.at(i);
            auto const& pending = baseCovers[i];
            auto const optDbData = resReader.get(cover.resourceId);
            auto const* baselineResourceId = std::get_if<ResourceId>(&pending.source);
            auto const* baselineData = std::get_if<std::span<std::byte const>>(&pending.source);

            if (pending.type != cover.type ||
                (baselineResourceId != nullptr && *baselineResourceId != cover.resourceId) ||
                (baselineData != nullptr && (!optDbData || !std::ranges::equal(*optDbData, *baselineData))))
            {
              shouldExportCovers = true;
            }
          }
        }
      }

      if (!shouldExportCovers)
      {
        return;
      }

      auto coversNode = node.append_child();
      yaml::setKey(coversNode, "covers");
      coversNode |= ryml::SEQ;

      for (std::uint16_t i = 0; i < coverCount; ++i)
      {
        auto const cover = covers.at(i);
        auto const resId = cover.resourceId;
        auto const typeValue = static_cast<std::uint8_t>(cover.type);

        auto coverNode = coversNode.append_child();
        coverNode |= ryml::MAP;

        coverNode.append_child() << ryml::key("type") << static_cast<std::uint32_t>(typeValue);

        if (auto const it = exportedCovers.find(resId); it != exportedCovers.end())
        {
          auto dataNode = coverNode.append_child();
          dataNode << ryml::key("data");
          dataNode.set_val_ref(yaml::copyToArena(dataNode, it->second));
        }
        else
        {
          if (auto const optData = resReader.get(resId); optData && !optData->empty())
          {
            auto const b64 = utility::base64Encode(*optData);
            auto const anchorName = "cover_" + std::to_string(resId.raw());

            auto dataNode = coverNode.append_child();
            dataNode << ryml::key("data") << b64;
            dataNode.set_val_anchor(yaml::copyToArena(dataNode, anchorName));
            exportedCovers[resId] = anchorName;
          }
        }
      }
    }

    void emitTrackCommon(ryml::NodeRef& node, library::TrackView::TagProxy const& tags, library::DictionaryStore& dict)
    {
      if (tags.count() != 0)
      {
        auto tagsNode = node.append_child();
        yaml::setKey(tagsNode, "tags");
        tagsNode |= ryml::SEQ;

        for (auto const tagId : tags)
        {
          yaml::setValue(tagsNode.append_child(), dict.get(tagId));
        }
      }
    }
  } // namespace

  struct LibraryYamlExporter::Impl final
  {
    explicit Impl(library::MusicLibrary& ml)
      : ml{ml}
    {
    }

    Result<> exportToYaml(std::filesystem::path const& path, ExportMode mode) const;
    void exportTracks(ryml::NodeRef& node, lmdb::ReadTransaction const& txn, ExportMode mode) const;
    void exportTrack(ryml::NodeRef& node,
                     lmdb::ReadTransaction const& txn,
                     TrackId id,
                     library::TrackView const& view,
                     ExportMode mode,
                     std::unordered_map<ResourceId, std::string>& exportedCovers,
                     library::ResourceStore& resources,
                     library::DictionaryStore& dict,
                     library::FileManifestStore::Reader const& manifestReader) const;
    void exportLists(ryml::NodeRef& node, lmdb::ReadTransaction const& txn, ExportMode mode) const;

    library::MusicLibrary& ml;
  };

  LibraryYamlExporter::LibraryYamlExporter(library::MusicLibrary& ml)
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

    root.append_child() << ryml::key("version") << 1;
    appendString(root, "libraryId", formatUuid(ml.metaHeader().libraryId));
    appendString(root, "export_mode", modeToString(mode));

    auto const txn = ml.readTransaction();
    auto library = root.append_child();
    yaml::setKey(library, "library");
    library |= ryml::MAP;

    if (mode != ExportMode::ListOnly)
    {
      exportTracks(library, txn, mode);
    }

    exportLists(library, txn, mode);

    auto ofs = std::ofstream{path};

    if (!ofs)
    {
      return makeError(Error::Code::IoError, std::format("Failed to open '{}' for writing", path.string()));
    }

    std::string const yaml = ryml::emitrs_yaml<std::string>(tree);
    ofs << yaml;

    if (!ofs.good())
    {
      return makeError(Error::Code::IoError, std::format("File write error while writing '{}'", path.string()));
    }

    return {};
  }

  void LibraryYamlExporter::Impl::exportTracks(ryml::NodeRef& node,
                                               lmdb::ReadTransaction const& txn,
                                               ExportMode mode) const
  {
    auto const trackReader = ml.tracks().reader(txn);
    auto const manifestReader = ml.manifest().reader(txn);
    auto& resources = ml.resources();
    auto& dict = ml.dictionary();
    auto exportedCovers = std::unordered_map<ResourceId, std::string>{};

    auto tracksNode = node.append_child();
    yaml::setKey(tracksNode, "tracks");
    tracksNode |= ryml::SEQ;

    for (auto const& [trackId, view] : trackReader)
    {
      exportTrack(tracksNode, txn, trackId, view, mode, exportedCovers, resources, dict, manifestReader);
    }
  }

  void LibraryYamlExporter::Impl::exportTrack(ryml::NodeRef& node,
                                              lmdb::ReadTransaction const& txn,
                                              TrackId id,
                                              library::TrackView const& view,
                                              ExportMode mode,
                                              std::unordered_map<ResourceId, std::string>& exportedCovers,
                                              library::ResourceStore& resources,
                                              library::DictionaryStore& dict,
                                              library::FileManifestStore::Reader const& manifestReader) const
  {
    auto trackNode = node.append_child();
    trackNode |= ryml::MAP;

    trackNode.append_child() << ryml::key("id") << id.raw();

    auto const property = view.property();
    appendString(trackNode, "uri", property.uri());

    auto optBaseline = std::optional<library::TrackBuilder>{};
    auto tagFilePtr = std::unique_ptr<tag::TagFile>{};

    if (mode == ExportMode::Delta)
    {
      if (auto const fullPath = ml.rootPath() / property.uri(); std::filesystem::exists(fullPath))
      {
        tagFilePtr = tag::TagFile::open(fullPath);

        if (tagFilePtr)
        {
          optBaseline = tagFilePtr->loadTrack();
        }
      }
    }

    if (mode != ExportMode::ListOnly)
    {
      emitTrackMetadata(trackNode, view, dict, optBaseline);
    }

    if (mode == ExportMode::Full)
    {
      emitTrackProperties(trackNode, property, manifestReader);
    }

    emitTrackCover(trackNode, txn, view, optBaseline, mode, exportedCovers, resources);
    emitTrackCommon(trackNode, view.tags(), dict);
  }

  void LibraryYamlExporter::Impl::exportLists(ryml::NodeRef& node,
                                              lmdb::ReadTransaction const& txn,
                                              ExportMode mode) const
  {
    auto listsNode = node.append_child();
    yaml::setKey(listsNode, "lists");
    listsNode |= ryml::SEQ;

    auto const listReader = ml.lists().reader(txn);
    auto const trackReader = ml.tracks().reader(txn);

    for (auto const& [listId, listView] : listReader)
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
      }
      else
      {
        if (auto const tracks = listView.tracks(); !tracks.empty())
        {
          auto tracksSeqNode = listNode.append_child();
          yaml::setKey(tracksSeqNode, "tracks");
          tracksSeqNode |= ryml::SEQ;

          for (auto const tid : tracks)
          {
            if (mode == ExportMode::ListOnly)
            {
              if (auto const optTrackView = trackReader.get(tid); optTrackView)
              {
                auto refNode = tracksSeqNode.append_child();
                refNode |= ryml::MAP;
                appendString(refNode, "uri", optTrackView->property().uri());
              }
            }
            else
            {
              tracksSeqNode.append_child() << tid.raw();
            }
          }
        }
      }
    }
  }
} // namespace ao::rt
