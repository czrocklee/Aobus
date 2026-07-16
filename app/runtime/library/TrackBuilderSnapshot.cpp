// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackBuilderSnapshot.h"

#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace ao::rt
{
  TrackBuilderSnapshot::TrackBuilderSnapshot(library::TrackBuilder const& source)
    : _title{source.metadata().title()}
    , _artist{source.metadata().artist()}
    , _album{source.metadata().album()}
    , _albumArtist{source.metadata().albumArtist()}
    , _composer{source.metadata().composer()}
    , _conductor{source.metadata().conductor()}
    , _ensemble{source.metadata().ensemble()}
    , _genre{source.metadata().genre()}
    , _work{source.metadata().work()}
    , _movement{source.metadata().movement()}
    , _soloist{source.metadata().soloist()}
    , _uri{source.property().uri()}
    , _year{source.metadata().year()}
    , _trackNumber{source.metadata().trackNumber()}
    , _trackTotal{source.metadata().trackTotal()}
    , _discNumber{source.metadata().discNumber()}
    , _discTotal{source.metadata().discTotal()}
    , _movementNumber{source.metadata().movementNumber()}
    , _movementTotal{source.metadata().movementTotal()}
    , _duration{source.property().duration()}
    , _bitrate{source.property().bitrate()}
    , _sampleRate{source.property().sampleRate()}
    , _codec{source.property().codec()}
    , _channels{source.property().channels()}
    , _bitDepth{source.property().bitDepth()}
  {
    _tags.reserve(source.tags().names().size());

    for (auto const tag : source.tags().names())
    {
      _tags.emplace_back(tag);
    }

    _covers.reserve(source.coverArt().entries().size());

    for (auto const& entry : source.coverArt().entries())
    {
      if (auto const* const resourceId = std::get_if<ResourceId>(&entry.source); resourceId != nullptr)
      {
        _covers.push_back(Cover{.type = entry.type, .source = *resourceId});
      }
      else
      {
        auto const bytes = std::get<std::span<std::byte const>>(entry.source);
        _covers.push_back(Cover{.type = entry.type, .source = std::vector<std::byte>{bytes.begin(), bytes.end()}});
      }
    }

    _customMetadata.reserve(source.customMetadata().pairs().size());

    for (auto const& [key, value] : source.customMetadata().pairs())
    {
      _customMetadata.emplace_back(key, value);
    }
  }

  library::TrackBuilder TrackBuilderSnapshot::makeBuilder() const
  {
    auto result = library::TrackBuilder::makeEmpty();
    result.metadata()
      .title(_title)
      .artist(_artist)
      .album(_album)
      .albumArtist(_albumArtist)
      .composer(_composer)
      .conductor(_conductor)
      .ensemble(_ensemble)
      .genre(_genre)
      .work(_work)
      .movement(_movement)
      .soloist(_soloist)
      .year(_year)
      .trackNumber(_trackNumber)
      .trackTotal(_trackTotal)
      .discNumber(_discNumber)
      .discTotal(_discTotal)
      .movementNumber(_movementNumber)
      .movementTotal(_movementTotal);
    result.property()
      .uri(_uri)
      .duration(_duration)
      .bitrate(_bitrate)
      .sampleRate(_sampleRate)
      .codec(_codec)
      .channels(_channels)
      .bitDepth(_bitDepth);

    for (auto const& tag : _tags)
    {
      result.tags().add(tag);
    }

    for (auto const& cover : _covers)
    {
      if (auto const* const resourceId = std::get_if<ResourceId>(&cover.source); resourceId != nullptr)
      {
        result.coverArt().add(cover.type, *resourceId);
      }
      else
      {
        auto const& bytes = std::get<std::vector<std::byte>>(cover.source);
        result.coverArt().add(cover.type, std::span<std::byte const>{bytes});
      }
    }

    for (auto const& [key, value] : _customMetadata)
    {
      result.customMetadata().add(key, value);
    }

    return result;
  }
} // namespace ao::rt
