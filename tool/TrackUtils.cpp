// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackUtils.h"
#include <rs/tag/File.h>

rs::core::TrackBuilder loadTrackRecord(std::filesystem::path const& path,
                                       rs::core::DictionaryStore& dictionary,
                                       rs::core::ResourceStore::Writer& resourceWriter,
                                       rs::lmdb::WriteTransaction& txn)
{
  auto tagFile = rs::tag::File::open(path);
  if (!tagFile) { return rs::core::TrackBuilder::createNew(); }

  auto parsed = tagFile->loadTrack();

  // Fill in library context
  parsed.record.metadata.uri = path.string();
  parsed.record.property.fileSize = std::filesystem::file_size(path);
  parsed.record.property.mtime = std::filesystem::last_write_time(path).time_since_epoch().count();

  // Store cover art
  if (parsed.embeddedCoverArt.empty())
  {
    parsed.record.metadata.coverArtId = resourceWriter.create(parsed.embeddedCoverArt).value();
  }

  return rs::core::TrackBuilder::fromRecord(std::move(parsed.record));
}
