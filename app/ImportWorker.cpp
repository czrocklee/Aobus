// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ImportWorker.h"

#include <rs/core/TrackBuilder.h>
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <algorithm>

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace
{
  std::unique_ptr<rs::tag::File> createTagFileByExtension(std::filesystem::path const& path)
  {
    using namespace rs::tag;
    static std::unordered_map<std::string, std::function<std::unique_ptr<File>(std::filesystem::path const)>> const
      CreatorMap = {
        {".mp3", [](auto const& path) { return std::make_unique<mpeg::File>(path, File::Mode::ReadOnly); }},
        {".m4a", [](auto const& path) { return std::make_unique<mp4::File>(path, File::Mode::ReadOnly); }},
        {".flac", [](auto const& path) { return std::make_unique<flac::File>(path, File::Mode::ReadOnly); }}};

    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (auto it = CreatorMap.find(ext); it != CreatorMap.end()) { return it->second(path); }
    return nullptr;
  }
} // namespace

ImportWorker::ImportWorker(rs::core::MusicLibrary& ml,
                           std::vector<std::filesystem::path> const& files,
                           ProgressCallback progressCallback,
                           FinishedCallback finishedCallback)
  : _ml{ml}
  , _files{files}
  , _progressCallback{progressCallback}
  , _finishedCallback{finishedCallback}
  , _rootPathStr{ml.rootPath().string()}
{
}

ImportWorker::~ImportWorker() = default;

void ImportWorker::run()
{
  auto txn = _ml.writeTransaction();
  auto trackWriter = _ml.tracks().writer(txn);
  auto resourceWriter = _ml.resources().writer(txn);
  auto& dict = _ml.dictionary();

  for (auto i = 0u; i < _files.size(); ++i)
  {
    try
    {
      auto const& path = _files[i];

      // Report progress
      if (_progressCallback) { _progressCallback(path, static_cast<std::int32_t>(i)); }

      // Process the file
      auto tagFile = createTagFileByExtension(path);
      if (!tagFile)
      {
        ++_result.skippedCount;
        continue;
      }

      rs::tag::Metadata metadata;
      metadata = tagFile->loadMetadata();

      // Build track from metadata
      auto builder = populateRecord(metadata, path, dict, resourceWriter);

      // Serialize hot and cold data
      auto [hotData, coldData] = builder.serialize(dict, txn);

      // Create the track
      auto [trackId, view] = trackWriter.createHotCold(hotData, coldData);
      _result.insertedIds.push_back(trackId);
    }
    catch ([[maybe_unused]] std::exception const& e)
    {
      ++_result.failureCount;
      continue;
    }
  }

  // Commit the transaction
  txn.commit();

  // Call finished callback
  if (_finishedCallback) { _finishedCallback(); }
}

void ImportWorker::join()
{
  if (_workerThread.joinable()) { _workerThread.join(); }
}

rs::core::TrackBuilder ImportWorker::populateRecord(rs::tag::Metadata const& metadata,
                                                    std::filesystem::path const& path,
                                                    rs::core::DictionaryStore& dict,
                                                    rs::core::ResourceStore::Writer& resourceWriter)
{
  auto builder = rs::core::TrackBuilder::createNew();

  // Populate metadata
  auto metaBuilder = builder.metadata();

  // Title
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::Title)))
  {
    metaBuilder.title(std::get<std::string>(metadata.get(rs::tag::MetaField::Title)));
  }

  // URI - store relative to library root
  auto relativePath = std::filesystem::relative(path, _ml.rootPath());
  metaBuilder.uri(relativePath.string());

  // Artist
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::Artist)))
  {
    metaBuilder.artist(std::get<std::string>(metadata.get(rs::tag::MetaField::Artist)));
  }

  // Album
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::Album)))
  {
    metaBuilder.album(std::get<std::string>(metadata.get(rs::tag::MetaField::Album)));
  }

  // Album Artist
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::AlbumArtist)))
  {
    metaBuilder.albumArtist(std::get<std::string>(metadata.get(rs::tag::MetaField::AlbumArtist)));
  }

  // Genre
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::Genre)))
  {
    metaBuilder.genre(std::get<std::string>(metadata.get(rs::tag::MetaField::Genre)));
  }

  // Year
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::Year)))
  {
    metaBuilder.year(static_cast<std::uint16_t>(std::get<std::int64_t>(metadata.get(rs::tag::MetaField::Year))));
  }

  // Track number
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::TrackNumber)))
  {
    metaBuilder.trackNumber(
      static_cast<std::uint16_t>(std::get<std::int64_t>(metadata.get(rs::tag::MetaField::TrackNumber))));
  }

  // Total tracks
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::TotalTracks)))
  {
    metaBuilder.totalTracks(
      static_cast<std::uint16_t>(std::get<std::int64_t>(metadata.get(rs::tag::MetaField::TotalTracks))));
  }

  // Disc number
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::DiscNumber)))
  {
    metaBuilder.discNumber(static_cast<std::uint16_t>(std::get<std::int64_t>(metadata.get(rs::tag::MetaField::DiscNumber))));
  }

  // Total discs
  if (!rs::tag::isNull(metadata.get(rs::tag::MetaField::TotalDiscs)))
  {
    metaBuilder.totalDiscs(static_cast<std::uint16_t>(std::get<std::int64_t>(metadata.get(rs::tag::MetaField::TotalDiscs))));
  }

  // Cover art - store in ResourceStore and get ID
  if (auto albumArt = metadata.get(rs::tag::MetaField::AlbumArt); !rs::tag::isNull(albumArt))
  {
    auto const& blob = std::get<rs::tag::Blob>(albumArt);
    auto artBytes = std::vector<std::byte>(blob.size());
    std::transform(blob.begin(), blob.end(), artBytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    auto resourceId = resourceWriter.create(artBytes);
    metaBuilder.coverArtId(resourceId.value());
  }

  // File properties
  auto propBuilder = builder.property();

  // File size
  if (std::filesystem::exists(path)) { propBuilder.fileSize(std::filesystem::file_size(path)); }

  // Modification time
  auto ftime = std::filesystem::last_write_time(path);
  auto epochInNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
  propBuilder.mtime(static_cast<std::uint64_t>(epochInNanos));

  return builder;
}