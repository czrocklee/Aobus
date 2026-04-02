// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackUtils.h"
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <algorithm>
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
} // namespace

core::TrackBuilder loadTrackRecord(std::filesystem::path const& path,
                                  core::DictionaryStore& dictionary,
                                  core::ResourceStore::Writer& resourceWriter,
                                  lmdb::WriteTransaction& txn)
{
  auto const file = createTagFileByExtension(path);
  auto const metadata = file->loadMetadata();

  auto builder = core::TrackBuilder::createNew();

  // Populate metadata
  auto metaBuilder = builder.metadata();

  if (auto const& titleVal = metadata.get(tag::MetaField::Title); !tag::isNull(titleVal))
  {
    metaBuilder.title(getString(titleVal));
  }

  if (auto const& artistVal = metadata.get(tag::MetaField::Artist); !tag::isNull(artistVal))
  {
    metaBuilder.artist(getString(artistVal));
  }

  if (auto const& albumVal = metadata.get(tag::MetaField::Album); !tag::isNull(albumVal))
  {
    metaBuilder.album(getString(albumVal));
  }

  if (auto const& genreVal = metadata.get(tag::MetaField::Genre); !tag::isNull(genreVal))
  {
    metaBuilder.genre(getString(genreVal));
  }

  if (auto const& year = metadata.get(tag::MetaField::Year); !tag::isNull(year))
  {
    metaBuilder.year(static_cast<std::uint16_t>(std::get<std::int64_t>(year)));
  }

  if (auto const& trackNum = metadata.get(tag::MetaField::TrackNumber); !tag::isNull(trackNum))
  {
    metaBuilder.trackNumber(static_cast<std::uint16_t>(std::get<std::int64_t>(trackNum)));
  }

  if (auto const& totalTracks = metadata.get(tag::MetaField::TotalTracks); !tag::isNull(totalTracks))
  {
    metaBuilder.totalTracks(static_cast<std::uint16_t>(std::get<std::int64_t>(totalTracks)));
  }

  if (auto const& discNum = metadata.get(tag::MetaField::DiscNumber); !tag::isNull(discNum))
  {
    metaBuilder.discNumber(static_cast<std::uint16_t>(std::get<std::int64_t>(discNum)));
  }

  if (auto const& totalDiscs = metadata.get(tag::MetaField::TotalDiscs); !tag::isNull(totalDiscs))
  {
    metaBuilder.totalDiscs(static_cast<std::uint16_t>(std::get<std::int64_t>(totalDiscs)));
  }

  // Cover art
  if (auto albumArt = metadata.get(tag::MetaField::AlbumArt); !tag::isNull(albumArt))
  {
    auto const& blob = std::get<tag::Blob>(albumArt);
    std::vector<std::byte> artBytes(blob.size());
    std::transform(blob.begin(), blob.end(), artBytes.begin(),
                  [](char c) { return static_cast<std::byte>(c); });
    auto resourceId = resourceWriter.create(artBytes);
    metaBuilder.coverArtId(resourceId.value());
  }

  metaBuilder.uri(path.string());

  // Populate property
  auto propBuilder = builder.property();
  propBuilder.fileSize(std::filesystem::file_size(path));
  propBuilder.mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

  return builder;
}
