// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackBuilderSnapshot.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/TrackBuilder.h>
#include <ao/utility/Sha256.h>

#include <cstddef>
#include <cstdint>
#include <format>
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

    _customMetadata.reserve(source.customMetadata().pairs().size());

    for (auto const& [key, value] : source.customMetadata().pairs())
    {
      _customMetadata.emplace_back(key, value);
    }
  }

  Result<TrackBuilderSnapshot> TrackBuilderSnapshot::make(library::TrackBuilder const& source)
  {
    auto result = TrackBuilderSnapshot{source};
    result._covers.reserve(source.coverArt().entries().size());

    for (auto const& entry : source.coverArt().entries())
    {
      if (auto const* const resourceId = std::get_if<ResourceId>(&entry.source); resourceId != nullptr)
      {
        result._covers.push_back(Cover{.type = entry.type, .source = *resourceId});
      }
      else if (auto const* const observed = std::get_if<library::ObservedResourceDescriptor>(&entry.source);
               observed != nullptr)
      {
        result._covers.push_back(Cover{.type = entry.type, .source = *observed});
      }
      else if (auto const* const descriptor = std::get_if<library::ResourceDescriptor>(&entry.source);
               descriptor != nullptr)
      {
        result._covers.push_back(Cover{.type = entry.type, .source = *descriptor});
      }
      else
      {
        auto const bytes = std::get<std::span<std::byte const>>(entry.source);

        if (!library::resourceByteLengthFits(bytes.size()))
        {
          return makeError(Error::Code::ValueTooLarge,
                           std::format("Resource content of {} bytes exceeds the stored length field", bytes.size()));
        }

        result._covers.push_back(Cover{
          .type = entry.type,
          .source =
            library::ObservedResourceDescriptor{
              .descriptor =
                library::ResourceDescriptor{
                  .digest = utility::computeSha256(bytes),
                  .byteLength = static_cast<std::uint32_t>(bytes.size()),
                },
            },
        });
      }
    }

    return result;
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
      else if (auto const* const observed = std::get_if<library::ObservedResourceDescriptor>(&cover.source);
               observed != nullptr)
      {
        result.coverArt().add(cover.type, *observed);
      }
      else
      {
        result.coverArt().add(cover.type, std::get<library::ResourceDescriptor>(cover.source));
      }
    }

    for (auto const& [key, value] : _customMetadata)
    {
      result.customMetadata().add(key, value);
    }

    return result;
  }
} // namespace ao::rt
