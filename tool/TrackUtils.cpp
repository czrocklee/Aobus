// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackUtils.h"
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <iostream>

namespace
{
  using namespace rs;

  std::unique_ptr<tag::File> createTagFileByExtension(std::filesystem::path const& path)
  {
    static auto const CreatorMap =
      std::unordered_map<std::string, std::function<std::unique_ptr<tag::File>(std::filesystem::path const)>>{
        {".mp3", [](auto const& path) { return std::make_unique<tag::mpeg::File>(path, tag::File::Mode::ReadOnly); }},
        {".m4a", [](auto const& path) { return std::make_unique<tag::mp4::File>(path, tag::File::Mode::ReadOnly); }},
        {".flac", [](auto const& path) { return std::make_unique<tag::flac::File>(path, tag::File::Mode::ReadOnly); }}};

    return std::invoke(CreatorMap.at(path.extension().string()), path);
  }

  std::string getString(tag::ValueType const& val)
  {
    return tag::isNull(val) ? std::string{} : std::get<std::string>(val);
  }

  void populateProperty(core::TrackRecord& record, std::filesystem::path const& path)
  {
    record.property.fileSize = std::filesystem::file_size(path);
    record.property.mtime = std::filesystem::last_write_time(path).time_since_epoch().count();
  }

  void populateMetadataFields(core::TrackRecord::Metadata& meta, tag::Metadata const& metadata)
  {
    if (auto const& titleVal = metadata.get(tag::MetaField::Title); !tag::isNull(titleVal))
    {
      meta.title = getString(titleVal);
    }

    if (auto const& artistVal = metadata.get(tag::MetaField::Artist); !tag::isNull(artistVal))
    {
      meta.artist = getString(artistVal);
    }

    if (auto const& albumVal = metadata.get(tag::MetaField::Album); !tag::isNull(albumVal))
    {
      meta.album = getString(albumVal);
    }

    if (auto const& genreVal = metadata.get(tag::MetaField::Genre); !tag::isNull(genreVal))
    {
      meta.genre = getString(genreVal);
    }

    if (auto const& year = metadata.get(tag::MetaField::Year); !tag::isNull(year))
    {
      meta.year = static_cast<std::uint16_t>(std::get<std::int64_t>(year));
    }

    if (auto const& trackNum = metadata.get(tag::MetaField::TrackNumber); !tag::isNull(trackNum))
    {
      meta.trackNumber = static_cast<std::uint16_t>(std::get<std::int64_t>(trackNum));
    }

    if (auto const& totalTracks = metadata.get(tag::MetaField::TotalTracks); !tag::isNull(totalTracks))
    {
      meta.totalTracks = static_cast<std::uint16_t>(std::get<std::int64_t>(totalTracks));
    }

    if (auto const& discNum = metadata.get(tag::MetaField::DiscNumber); !tag::isNull(discNum))
    {
      meta.discNumber = static_cast<std::uint16_t>(std::get<std::int64_t>(discNum));
    }

    if (auto const& totalDiscs = metadata.get(tag::MetaField::TotalDiscs); !tag::isNull(totalDiscs))
    {
      meta.totalDiscs = static_cast<std::uint16_t>(std::get<std::int64_t>(totalDiscs));
    }
  }

  void populateMetadata(core::TrackRecord& record,
                        tag::Metadata const& metadata,
                        std::filesystem::path const& path,
                        core::DictionaryStore& dictionary,
                        lmdb::WriteTransaction& txn)
  {
    record.metadata.uri = path.string();
    populateMetadataFields(record.metadata, metadata);

    if (!record.metadata.artist.empty()) { record.artistId = dictionary.put(txn, record.metadata.artist); }
    if (!record.metadata.album.empty()) { record.albumId = dictionary.put(txn, record.metadata.album); }
    if (!record.metadata.genre.empty()) { record.genreId = dictionary.put(txn, record.metadata.genre); }
  }
}

core::TrackRecord loadTrackRecord(std::filesystem::path const& path,
                                  core::DictionaryStore& dictionary,
                                  lmdb::WriteTransaction& txn)
{
  auto const file = createTagFileByExtension(path);
  auto const metadata = file->loadMetadata();
  auto record = core::TrackRecord{};
  populateProperty(record, path);
  populateMetadata(record, metadata, path, dictionary, txn);
  return record;
}
