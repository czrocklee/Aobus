// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/LibraryExporter.h"

#include "ao/Exception.h"
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

#include <yaml-cpp/yaml.h>

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

    std::string modeToString(ExportMode mode)
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
      MetadataStringGetter strGet;
      MetadataStringBaseGetter strBaseGet;
      MetadataNumberGetter numGet;
      MetadataNumberBaseGetter numBaseGet;
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

    void emitTrackMetadata(YAML::Emitter& out,
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
            out << YAML::Key << std::string(key) << YAML::Value << std::string(current);
          }
        }
        else if (map.numGet != nullptr)
        {
          auto const current = map.numGet(metadata);
          bool const shouldEmit = hasBaseline ? (current != map.numBaseGet(*optBaselineMeta)) : (current != 0);

          if (shouldEmit)
          {
            out << YAML::Key << std::string(key) << YAML::Value << current;
          }
        }
      }

      if (auto const custom = view.custom(); !custom.empty())
      {
        // Custom fields are always Aobus-specific deltas (not in standard tags)
        out << YAML::Key << "custom" << YAML::Value << YAML::BeginMap;

        for (auto const& [dictId, value] : custom)
        {
          out << YAML::Key << std::string(dict.get(dictId)) << YAML::Value << std::string(value);
        }

        out << YAML::EndMap;
      }
    }

    using PropertyU64Getter = std::uint64_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU32Getter = std::uint32_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU16Getter = std::uint16_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU8Getter = std::uint8_t (*)(library::TrackView::PropertyProxy const&);

    struct PropertyDispatch final
    {
      rt::TrackField field;
      PropertyU64Getter u64Get;
      PropertyU32Getter u32Get;
      PropertyU16Getter u16Get;
      PropertyU8Getter u8Get;
    };

    constexpr auto kPropertyDispatch = std::to_array<PropertyDispatch>({
      {.field = rt::TrackField::Duration, .u32Get = [](auto const& prop) { return prop.durationMs(); }},
      {.field = rt::TrackField::Bitrate, .u32Get = [](auto const& prop) { return prop.bitrate(); }},
      {.field = rt::TrackField::SampleRate, .u32Get = [](auto const& prop) { return prop.sampleRate(); }},
      {.field = rt::TrackField::Codec, .u16Get = [](auto const& prop) { return prop.codecId(); }},
      {.field = rt::TrackField::Channels, .u8Get = [](auto const& prop) { return prop.channels(); }},
      {.field = rt::TrackField::BitDepth, .u8Get = [](auto const& prop) { return prop.bitDepth(); }},
    });

    void emitTrackProperties(YAML::Emitter& out, library::TrackView::PropertyProxy const& property)
    {
      for (auto const& map : kPropertyDispatch)
      {
        if (auto const key = rt::trackFieldId(map.field); map.u64Get != nullptr)
        {
          out << YAML::Key << std::string(key) << YAML::Value << map.u64Get(property);
        }
        else if (map.u32Get != nullptr)
        {
          out << YAML::Key << std::string(key) << YAML::Value << map.u32Get(property);
        }
        else if (map.u16Get != nullptr)
        {
          out << YAML::Key << std::string(key) << YAML::Value << map.u16Get(property);
        }
        else if (map.u8Get != nullptr)
        {
          out << YAML::Key << std::string(key) << YAML::Value << static_cast<int>(map.u8Get(property));
        }
      }

      out << YAML::Key << "fileSize" << YAML::Value << property.fileSize();
      out << YAML::Key << "mtime" << YAML::Value << property.mtime();
    }

    void emitTrackCover(YAML::Emitter& out,
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
            out << YAML::Key << "coverArtBase64" << YAML::Value << YAML::Alias(it->second);
          }
          else
          {
            if (auto const optData = resources.reader(txn).get(rid); optData && !optData->empty())
            {
              auto const b64 = utility::base64Encode(*optData);
              auto const anchorName = "cover_" + std::to_string(rid);

              out << YAML::Key << "coverArtBase64" << YAML::Value << YAML::Anchor(anchorName) << b64;
              exportedCovers[resId] = anchorName;
            }
          }
        }
      }
    }

    void emitTrackCommon(YAML::Emitter& out,
                         library::TrackView::MetadataProxy const& metadata,
                         library::TrackView::TagProxy const& tags,
                         library::DictionaryStore& dict)
    {
      if (metadata.rating() != 0)
      {
        out << YAML::Key << "rating" << YAML::Value << static_cast<int>(metadata.rating());
      }

      if (tags.count() != 0)
      {
        out << YAML::Key << "tags" << YAML::Value << YAML::BeginSeq;

        for (auto const tagId : tags)
        {
          out << std::string(dict.get(tagId));
        }

        out << YAML::EndSeq;
      }
    }
  } // namespace

  struct LibraryExporter::Impl final
  {
    explicit Impl(library::MusicLibrary& ml)
      : ml{ml}
    {
    }

    void exportToYaml(std::filesystem::path const& path, ExportMode mode) const;
    void exportTracks(YAML::Emitter& out, lmdb::ReadTransaction const& txn, ExportMode mode) const;
    void exportTrack(YAML::Emitter& out,
                     lmdb::ReadTransaction const& txn,
                     TrackId id,
                     library::TrackView const& view,
                     ExportMode mode,
                     std::unordered_map<ResourceId, std::string>& exportedCovers,
                     library::ResourceStore& resources,
                     library::DictionaryStore& dict) const;
    void exportLists(YAML::Emitter& out, lmdb::ReadTransaction const& txn, ExportMode mode) const;

    library::MusicLibrary& ml;
  };

  LibraryExporter::LibraryExporter(library::MusicLibrary& ml)
    : _impl{std::make_unique<Impl>(ml)}
  {
  }

  LibraryExporter::~LibraryExporter() = default;

  void LibraryExporter::exportToYaml(std::filesystem::path const& path, ExportMode mode)
  {
    _impl->exportToYaml(path, mode);
  }

  void LibraryExporter::Impl::exportToYaml(std::filesystem::path const& path, ExportMode mode) const
  {
    auto ofs = std::ofstream{path};

    if (!ofs)
    {
      ao::throwException<Exception>("Failed to open '{}' for writing", path.string());
    }

    auto out = YAML::Emitter{ofs};
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << 1;
    out << YAML::Key << "libraryId" << YAML::Value << formatUuid(ml.metaHeader().libraryId);
    out << YAML::Key << "export_mode" << YAML::Value << modeToString(mode);

    auto const txn = ml.readTransaction();
    out << YAML::Key << "library" << YAML::Value << YAML::BeginMap;

    if (mode != ExportMode::ListOnly)
    {
      exportTracks(out, txn, mode);
    }

    exportLists(out, txn, mode);

    out << YAML::EndMap; // end library
    out << YAML::EndMap; // end root

    if (!out.good())
    {
      ao::throwException<Exception>("YAML emitter error while writing '{}': {}", path.string(), out.GetLastError());
    }
  }

  void LibraryExporter::Impl::exportTracks(YAML::Emitter& out, lmdb::ReadTransaction const& txn, ExportMode mode) const
  {
    auto const trackReader = ml.tracks().reader(txn);
    auto const manifestReader = ml.manifest().reader(txn);
    auto& resources = ml.resources();
    auto& dict = ml.dictionary();
    auto exportedCovers = std::unordered_map<ResourceId, std::string>{};

    out << YAML::Key << "tracks" << YAML::Value << YAML::BeginSeq;

    for (auto const& [trackId, view] : trackReader)
    {
      auto viewWithManifest = view;

      if (auto const optEntry = manifestReader.get(view.property().uri()))
      {
        viewWithManifest = library::TrackView{view.hotData(), view.coldData(), optEntry->fileSize(), optEntry->mtime()};
      }

      exportTrack(out, txn, trackId, viewWithManifest, mode, exportedCovers, resources, dict);
    }

    out << YAML::EndSeq;
  }

  void LibraryExporter::Impl::exportTrack(YAML::Emitter& out,
                                          lmdb::ReadTransaction const& txn,
                                          TrackId id,
                                          library::TrackView const& view,
                                          ExportMode mode,
                                          std::unordered_map<ResourceId, std::string>& exportedCovers,
                                          library::ResourceStore& resources,
                                          library::DictionaryStore& dict) const
  {
    out << YAML::BeginMap;
    out << YAML::Key << "id" << YAML::Value << id.raw();

    auto const property = view.property();
    out << YAML::Key << "uri" << YAML::Value << std::string(property.uri());

    auto const metadata = view.metadata();
    auto optBaseline = std::optional<library::TrackBuilder>{};

    if (mode == ExportMode::Delta)
    {
      // Delta mode: Diff against physical file
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
      emitTrackMetadata(out, view, dict, optBaseline);
    }

    if (mode == ExportMode::Full)
    {
      emitTrackProperties(out, property);
    }

    emitTrackCover(out, txn, metadata, optBaseline, mode, exportedCovers, resources);
    emitTrackCommon(out, metadata, view.tags(), dict);

    out << YAML::EndMap;
  }

  void LibraryExporter::Impl::exportLists(YAML::Emitter& out, lmdb::ReadTransaction const& txn, ExportMode mode) const
  {
    out << YAML::Key << "lists" << YAML::Value << YAML::BeginSeq;
    auto const listReader = ml.lists().reader(txn);
    auto const trackReader = ml.tracks().reader(txn);

    for (auto const& [listId, listView] : listReader)
    {
      out << YAML::BeginMap;
      out << YAML::Key << "id" << YAML::Value << listId.raw();
      out << YAML::Key << "parentId" << YAML::Value << listView.parentId().raw();
      out << YAML::Key << "name" << YAML::Value << std::string(listView.name());

      if (!listView.description().empty())
      {
        out << YAML::Key << "description" << YAML::Value << std::string(listView.description());
      }

      if (listView.isSmart())
      {
        out << YAML::Key << "filter" << YAML::Value << std::string(listView.filter());
      }
      else
      {
        if (auto const tracks = listView.tracks(); !tracks.empty())
        {
          out << YAML::Key << "tracks" << YAML::Value << YAML::BeginSeq;

          for (auto const tid : tracks)
          {
            if (mode == ExportMode::ListOnly)
            {
              if (auto const optTrack = trackReader.get(tid); optTrack)
              {
                out << YAML::BeginMap;
                out << YAML::Key << "uri" << YAML::Value << std::string(optTrack->property().uri());
                out << YAML::EndMap;
              }
            }
            else
            {
              out << tid.raw();
            }
          }

          out << YAML::EndSeq;
        }
      }

      out << YAML::EndMap;
    }

    out << YAML::EndSeq;
  }
} // namespace ao::rt
