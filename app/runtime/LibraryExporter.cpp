// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/LibraryExporter.h"

#include "ao/Error.h"
#include "ao/Type.h"
#include "ao/library/DictionaryStore.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/ListStore.h"
#include "ao/library/ListView.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/ResourceStore.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"
#include "ao/lmdb/Transaction.h"
#include "ao/tag/TagFile.h"
#include "ao/utility/Base64.h"
#include "runtime/TrackField.h"
#include "runtime/yaml/Utils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
      auto byteIndex = std::size_t{0};

      for (auto groupIndex = std::size_t{0}; groupIndex < kUuidGroupByteCounts.size(); ++groupIndex)
      {
        if (groupIndex > 0)
        {
          result.push_back('-');
        }

        for (auto groupByte = std::size_t{0}; groupByte < kUuidGroupByteCounts.at(groupIndex); ++groupByte)
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
      rt::TrackField field;
      MetadataStringGetter strGet = nullptr;
      MetadataStringBaseGetter strBaseGet = nullptr;
      MetadataNumberGetter numGet = nullptr;
      MetadataNumberBaseGetter numBaseGet = nullptr;
    };

    constexpr auto kMetadataDispatch = std::to_array<MetadataDispatch>({
      {.field = rt::TrackField::Title,
       .strGet = [](auto const& meta, auto&) { return meta.title(); },
       .strBaseGet = [](auto const& base) { return base.title(); }},
      {.field = rt::TrackField::Artist,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.artistId() != kInvalidDictionaryId ? dict.get(meta.artistId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.artist(); }},
      {.field = rt::TrackField::Album,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.albumId() != kInvalidDictionaryId ? dict.get(meta.albumId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.album(); }},
      {.field = rt::TrackField::AlbumArtist,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.albumArtistId() != kInvalidDictionaryId ? dict.get(meta.albumArtistId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.albumArtist(); }},
      {.field = rt::TrackField::Composer,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.composerId() != kInvalidDictionaryId ? dict.get(meta.composerId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.composer(); }},
      {.field = rt::TrackField::Genre,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.genreId() != kInvalidDictionaryId ? dict.get(meta.genreId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.genre(); }},
      {.field = rt::TrackField::Work,
       .strGet = [](auto const& meta, auto& dict)
       { return meta.workId() != kInvalidDictionaryId ? dict.get(meta.workId()) : std::string_view{}; },
       .strBaseGet = [](auto const& base) { return base.work(); }},
      {.field = rt::TrackField::Year,
       .numGet = [](auto const& meta) { return meta.year(); },
       .numBaseGet = [](auto const& base) { return base.year(); }},
      {.field = rt::TrackField::TrackNumber,
       .numGet = [](auto const& meta) { return meta.trackNumber(); },
       .numBaseGet = [](auto const& base) { return base.trackNumber(); }},
      {.field = rt::TrackField::TotalTracks,
       .numGet = [](auto const& meta) { return meta.totalTracks(); },
       .numBaseGet = [](auto const& base) { return base.totalTracks(); }},
      {.field = rt::TrackField::DiscNumber,
       .numGet = [](auto const& meta) { return meta.discNumber(); },
       .numBaseGet = [](auto const& base) { return base.discNumber(); }},
      {.field = rt::TrackField::TotalDiscs,
       .numGet = [](auto const& meta) { return meta.totalDiscs(); },
       .numBaseGet = [](auto const& base) { return base.totalDiscs(); }},
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
        if (auto const key = rt::trackFieldId(map.field); map.strGet != nullptr)
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

      if (auto const custom = view.custom(); !custom.empty())
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

    struct PropertyDispatch final
    {
      rt::TrackField field;
      PropertyU64Getter u64Get = nullptr;
      PropertyU32Getter u32Get = nullptr;
      PropertyU16Getter u16Get = nullptr;
      PropertyU8Getter u8Get = nullptr;
    };

    constexpr auto kPropertyDispatch = std::to_array<PropertyDispatch>({
      {.field = rt::TrackField::Duration, .u32Get = [](auto const& prop) { return prop.durationMs(); }},
      {.field = rt::TrackField::Bitrate, .u32Get = [](auto const& prop) { return prop.bitrate(); }},
      {.field = rt::TrackField::SampleRate, .u32Get = [](auto const& prop) { return prop.sampleRate(); }},
      {.field = rt::TrackField::Codec, .u16Get = [](auto const& prop) { return prop.codecId(); }},
      {.field = rt::TrackField::Channels, .u8Get = [](auto const& prop) { return prop.channels(); }},
      {.field = rt::TrackField::BitDepth, .u8Get = [](auto const& prop) { return prop.bitDepth(); }},
    });

    void emitTrackProperties(ryml::NodeRef& node, library::TrackView::PropertyProxy const& property)
    {
      for (auto const& map : kPropertyDispatch)
      {
        if (auto const key = rt::trackFieldId(map.field); map.u64Get != nullptr)
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
      }

      node.append_child() << ryml::key("fileSize") << property.fileSize();
      node.append_child() << ryml::key("mtime") << property.mtime();
    }

    void emitTrackCover(ryml::NodeRef& node,
                        lmdb::ReadTransaction const& txn,
                        library::TrackView::MetadataProxy const& metadata,
                        std::optional<library::TrackBuilder> const& optBaseline,
                        ExportMode mode,
                        std::unordered_map<ResourceId, std::string>& exportedCovers,
                        library::ResourceStore& resources)
    {
      bool const isDelta = (mode == ExportMode::Delta);
      bool shouldExportCover = (mode == ExportMode::Metadata || mode == ExportMode::Full);

      if (isDelta && optBaseline)
      {
        if (metadata.coverArtId() != 0)
        {
          if (auto const optDbData = resources.reader(txn).get(metadata.coverArtId()); optDbData)
          {
            if (auto const fileData = optBaseline->metadata().coverArtData(); !std::ranges::equal(*optDbData, fileData))
            {
              shouldExportCover = true;
            }
          }
        }
      }

      if (shouldExportCover)
      {
        if (auto const rid = metadata.coverArtId(); rid != 0)
        {
          auto const resId = ResourceId{rid};

          if (auto const it = exportedCovers.find(resId); it != exportedCovers.end())
          {
            auto child = node.append_child();
            child << ryml::key("coverArtBase64");
            child.set_val_ref(yaml::copyToArena(child, it->second));
          }
          else
          {
            if (auto const optData = resources.reader(txn).get(rid); optData && !optData->empty())
            {
              auto const b64 = utility::base64Encode(*optData);
              auto const anchorName = "cover_" + std::to_string(rid);

              auto child = node.append_child();
              child << ryml::key("coverArtBase64") << node.tree()->to_arena(b64);
              child.set_val_anchor(yaml::copyToArena(child, anchorName));
              exportedCovers[resId] = anchorName;
            }
          }
        }
      }
    }

    void emitTrackCommon(ryml::NodeRef& node,
                         library::TrackView::MetadataProxy const& metadata,
                         library::TrackView::TagProxy const& tags,
                         library::DictionaryStore& dict)
    {
      if (metadata.rating() != 0)
      {
        node.append_child() << ryml::key("rating") << metadata.rating();
      }

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

  struct LibraryExporter::Impl final
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
                     library::DictionaryStore& dict) const;
    void exportLists(ryml::NodeRef& node, lmdb::ReadTransaction const& txn, ExportMode mode) const;

    library::MusicLibrary& ml;
  };

  LibraryExporter::LibraryExporter(library::MusicLibrary& ml)
    : _impl{std::make_unique<Impl>(ml)}
  {
  }

  LibraryExporter::~LibraryExporter() = default;

  Result<> LibraryExporter::exportToYaml(std::filesystem::path const& path, ExportMode mode)
  {
    return _impl->exportToYaml(path, mode);
  }

  Result<> LibraryExporter::Impl::exportToYaml(std::filesystem::path const& path, ExportMode mode) const
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

  void LibraryExporter::Impl::exportTracks(ryml::NodeRef& node, lmdb::ReadTransaction const& txn, ExportMode mode) const
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
      auto viewWithManifest = view;

      if (auto const optEntry = manifestReader.get(view.property().uri()))
      {
        viewWithManifest = library::TrackView{view.hotData(), view.coldData(), optEntry->fileSize(), optEntry->mtime()};
      }

      exportTrack(tracksNode, txn, trackId, viewWithManifest, mode, exportedCovers, resources, dict);
    }
  }

  void LibraryExporter::Impl::exportTrack(ryml::NodeRef& node,
                                          lmdb::ReadTransaction const& txn,
                                          TrackId id,
                                          library::TrackView const& view,
                                          ExportMode mode,
                                          std::unordered_map<ResourceId, std::string>& exportedCovers,
                                          library::ResourceStore& resources,
                                          library::DictionaryStore& dict) const
  {
    auto trackNode = node.append_child();
    trackNode |= ryml::MAP;

    trackNode.append_child() << ryml::key("id") << id.raw();

    auto const property = view.property();
    appendString(trackNode, "uri", property.uri());

    auto const metadata = view.metadata();
    auto optBaseline = std::optional<library::TrackBuilder>{};

    if (mode == ExportMode::Delta)
    {
      if (auto const fullPath = ml.rootPath() / property.uri(); std::filesystem::exists(fullPath))
      {
        if (auto tagFile = tag::TagFile::open(fullPath))
        {
          optBaseline = tagFile->loadTrack();
        }
      }
    }

    if (mode != ExportMode::ListOnly)
    {
      emitTrackMetadata(trackNode, view, dict, optBaseline);
    }

    if (mode == ExportMode::Full)
    {
      emitTrackProperties(trackNode, property);
    }

    emitTrackCover(trackNode, txn, metadata, optBaseline, mode, exportedCovers, resources);
    emitTrackCommon(trackNode, metadata, view.tags(), dict);
  }

  void LibraryExporter::Impl::exportLists(ryml::NodeRef& node, lmdb::ReadTransaction const& txn, ExportMode mode) const
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
              if (auto const optTrack = trackReader.get(tid); optTrack)
              {
                auto refNode = tracksSeqNode.append_child();
                refNode |= ryml::MAP;
                appendString(refNode, "uri", optTrack->property().uri());
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
