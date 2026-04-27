// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/LibraryExporter.h>

#include <rs/Exception.h>
#include <rs/core/DictionaryStore.h>
#include <rs/core/ListStore.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackStore.h>

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <map>
#include <unordered_map>

namespace rs::core
{

  namespace
  {
    std::string modeToString(ExportMode mode)
    {
      switch (mode)
      {
        case ExportMode::Minimum: return "minimum";
        case ExportMode::Metadata: return "metadata";
        case ExportMode::Full: return "full";
      }
      return "unknown";
    }
  }

  LibraryExporter::LibraryExporter(MusicLibrary& ml)
    : _ml{ml}
  {
  }

  void LibraryExporter::exportToYaml(std::filesystem::path const& path, ExportMode mode)
  {
    auto ofs = std::ofstream{path};

    if (!ofs)
    {
      RS_THROW_FORMAT(rs::Exception, "Failed to open '{}' for writing", path.string());
    }

    auto out = YAML::Emitter{ofs};
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << 1;
    out << YAML::Key << "export_mode" << YAML::Value << modeToString(mode);

    auto txn = _ml.readTransaction();
    out << YAML::Key << "library" << YAML::Value << YAML::BeginMap;

    exportTracks(out, txn, mode);
    exportLists(out, txn);

    out << YAML::EndMap; // end library
    out << YAML::EndMap; // end root

    if (!out.good())
    {
      RS_THROW_FORMAT(rs::Exception, "YAML emitter error while writing '{}': {}", path.string(), out.GetLastError());
    }
  }

  void LibraryExporter::exportTracks(YAML::Emitter& out, rs::lmdb::ReadTransaction& txn, ExportMode mode)
  {
    auto trackReader = _ml.tracks().reader(txn);
    out << YAML::Key << "tracks" << YAML::Value << YAML::BeginSeq;
    for (auto const& [trackId, view] : trackReader)
    {
      exportTrack(out, trackId, view, mode);
    }
    out << YAML::EndSeq;
  }

  void LibraryExporter::exportTrack(YAML::Emitter& out, TrackId id, TrackView const& view, ExportMode mode)
  {
    auto& dict = _ml.dictionary();
    out << YAML::BeginMap;
    out << YAML::Key << "id" << YAML::Value << id.value();
    auto const property = view.property();
    auto const metadata = view.metadata();
    out << YAML::Key << "uri" << YAML::Value << std::string(property.uri());

    // Metadata & Custom (Metadata or Full mode)

    if (mode == ExportMode::Metadata || mode == ExportMode::Full)
    {
      out << YAML::Key << "title" << YAML::Value << std::string(metadata.title());

      if (auto const artistId = metadata.artistId(); artistId != DictionaryId{0})
      {
        out << YAML::Key << "artist" << YAML::Value << std::string(dict.get(artistId));
      }

      auto const albumId = metadata.albumId();

      if (albumId != DictionaryId{0})
      {
        out << YAML::Key << "album" << YAML::Value << std::string(dict.get(albumId));
      }

      auto const albumArtistId = metadata.albumArtistId();

      if (albumArtistId != DictionaryId{0})
      {
        out << YAML::Key << "albumArtist" << YAML::Value << std::string(dict.get(albumArtistId));
      }

      auto const genreId = metadata.genreId();

      if (genreId != DictionaryId{0})
      {
        out << YAML::Key << "genre" << YAML::Value << std::string(dict.get(genreId));
      }

      if (metadata.year() != 0)
      {
        out << YAML::Key << "year" << YAML::Value << metadata.year();
      }

      if (metadata.trackNumber() != 0)
      {
        out << YAML::Key << "trackNumber" << YAML::Value << metadata.trackNumber();
      }

      if (metadata.totalTracks() != 0)
      {
        out << YAML::Key << "totalTracks" << YAML::Value << metadata.totalTracks();
      }

      if (metadata.discNumber() != 0)
      {
        out << YAML::Key << "discNumber" << YAML::Value << metadata.discNumber();
      }

      if (metadata.totalDiscs() != 0)
      {
        out << YAML::Key << "totalDiscs" << YAML::Value << metadata.totalDiscs();
      }

      // Custom metadata
      auto const custom = view.custom();

      if (!custom.empty())
      {
        out << YAML::Key << "custom" << YAML::Value << YAML::BeginMap;
        for (auto const& [dictId, value] : custom)
        {
          out << YAML::Key << std::string(dict.get(dictId)) << YAML::Value << std::string(value);
        }
        out << YAML::EndMap;
      }
    }

    // Full mode only: Properties & Cover Art

    if (mode == ExportMode::Full)
    {
      out << YAML::Key << "durationMs" << YAML::Value << property.durationMs();
      out << YAML::Key << "bitrate" << YAML::Value << property.bitrate();
      out << YAML::Key << "sampleRate" << YAML::Value << property.sampleRate();
      out << YAML::Key << "channels" << YAML::Value << (int)property.channels();
      out << YAML::Key << "bitDepth" << YAML::Value << (int)property.bitDepth();
      out << YAML::Key << "codecId" << YAML::Value << property.codecId();
      out << YAML::Key << "fileSize" << YAML::Value << property.fileSize();
      out << YAML::Key << "mtime" << YAML::Value << property.mtime();
    }

    // Common to ALL modes: Rating & Tags

    if (metadata.rating() != 0)
    {
      out << YAML::Key << "rating" << YAML::Value << static_cast<int>(metadata.rating());
    }

    auto const tags = view.tags();

    if (!tags.empty())
    {
      out << YAML::Key << "tags" << YAML::Value << YAML::BeginSeq;
      for (auto const tagId : tags)
      {
        out << std::string(dict.get(tagId));
      }
      out << YAML::EndSeq;
    }

    out << YAML::EndMap;
  }

  void LibraryExporter::exportLists(YAML::Emitter& out, rs::lmdb::ReadTransaction& txn)
  {
    out << YAML::Key << "lists" << YAML::Value << YAML::BeginSeq;
    auto listReader = _ml.lists().reader(txn);
    for (auto const& [listId, listView] : listReader)
    {
      out << YAML::BeginMap;
      out << YAML::Key << "id" << YAML::Value << listId.value();
      out << YAML::Key << "parentId" << YAML::Value << listView.parentId().value();
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
        auto const tracks = listView.tracks();

        if (!tracks.empty())
        {
          out << YAML::Key << "tracks" << YAML::Value << YAML::BeginSeq;
          for (auto const tid : tracks)
          {
            out << tid.value();
          }
          out << YAML::EndSeq;
        }
      }

      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
  }

} // namespace rs::core
