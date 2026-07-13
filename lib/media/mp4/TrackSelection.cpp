// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/TrackSelection.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::media::mp4
{
  namespace
  {
    constexpr std::size_t kHandlerTypeOffset = 8;
    constexpr std::size_t kHandlerTypeSize = 4;

    struct StsdBodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint32_buf_t entryCount;
    };

    static_assert(sizeof(StsdBodyLayout) == 8);
    static_assert(alignof(StsdBodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<StsdBodyLayout>);

    constexpr auto kTrackHdlrPath = std::to_array<std::string_view>({
      "trak",
      "mdia",
      "hdlr",
    });

    constexpr auto kTrackStsdPath = std::to_array<std::string_view>({
      "trak",
      "mdia",
      "minf",
      "stbl",
      "stsd",
    });

    Result<std::string_view> trackHandlerType(AtomView const& track)
    {
      auto nodeResult = findAtom(track, kTrackHdlrPath);

      if (!nodeResult)
      {
        return std::unexpected{nodeResult.error()};
      }

      if (!*nodeResult)
      {
        return makeError(Error::Code::NotFound, "MP4 track has no handler atom");
      }

      auto const payload = (*nodeResult)->payload();

      if (payload.size() < kHandlerTypeOffset + kHandlerTypeSize)
      {
        return makeError(Error::Code::NotFound, "MP4 track handler has no handler type");
      }

      return utility::bytes::stringView(payload.subspan(kHandlerTypeOffset, kHandlerTypeSize));
    }

    Result<std::string> firstSampleEntryType(AtomView const& stsdView)
    {
      auto const payload = stsdView.payload();
      auto const* const stsdLayout = utility::bytes::tryLayout<StsdBodyLayout>(payload);

      if (stsdLayout == nullptr || stsdLayout->entryCount.value() != 1)
      {
        return makeError(Error::Code::NotFound, "MP4 sample description has no single entry");
      }

      auto cursor = stsdView.children();
      auto entryResult = cursor.next();

      if (!entryResult)
      {
        return std::unexpected{entryResult.error()};
      }

      if (!*entryResult)
      {
        return makeError(Error::Code::NotFound, "MP4 sample description entry is missing");
      }

      return std::string{(**entryResult).type()};
    }

    bool isSupportedAudioSampleEntry(std::string_view sampleEntryType) noexcept
    {
      return sampleEntryType == "alac" || sampleEntryType == "mp4a";
    }

    Result<AudioTrackSelection> selectTrack(AtomView const& track, std::string_view targetSampleEntryType)
    {
      auto stsdResult = findAtom(track, kTrackStsdPath);

      if (!stsdResult)
      {
        return std::unexpected{stsdResult.error()};
      }

      if (!*stsdResult)
      {
        return makeError(Error::Code::NotFound, "MP4 track has no sample description");
      }

      auto sampleEntryTypeResult = firstSampleEntryType(**stsdResult);

      if (!sampleEntryTypeResult)
      {
        return std::unexpected{sampleEntryTypeResult.error()};
      }

      auto sampleEntryType = std::move(*sampleEntryTypeResult);

      if (!targetSampleEntryType.empty() && sampleEntryType != targetSampleEntryType)
      {
        return makeError(Error::Code::NotFound, "MP4 track sample entry does not match");
      }

      if (auto handlerTypeResult = trackHandlerType(track); !handlerTypeResult)
      {
        if (handlerTypeResult.error().code != Error::Code::NotFound)
        {
          return std::unexpected{handlerTypeResult.error()};
        }

        if (!isSupportedAudioSampleEntry(sampleEntryType))
        {
          return makeError(Error::Code::NotFound, "MP4 track is not recognizable as audio");
        }
      }
      else if (*handlerTypeResult != "soun")
      {
        return makeError(Error::Code::NotFound, "MP4 track handler is not audio");
      }

      return AudioTrackSelection{
        .track = track,
        .stsd = **stsdResult,
        .sampleEntryType = std::move(sampleEntryType),
      };
    }

    Result<AudioTrackSelection> findAudioTrackInMovie(AtomView const& movie, std::string_view targetSampleEntryType)
    {
      auto trackCursor = movie.children();

      while (true)
      {
        auto trackResult = trackCursor.next();

        if (!trackResult)
        {
          return std::unexpected{trackResult.error()};
        }

        if (!*trackResult)
        {
          return makeError(Error::Code::NotFound, "MP4 movie has no matching audio track");
        }

        auto const& track = **trackResult;

        if (track.type() != "trak")
        {
          continue;
        }

        auto selectionResult = selectTrack(track, targetSampleEntryType);

        if (selectionResult)
        {
          return selectionResult;
        }

        if (selectionResult.error().code != Error::Code::NotFound)
        {
          return std::unexpected{selectionResult.error()};
        }
      }
    }
  } // namespace

  Result<AudioTrackSelection> findAudioTrack(AtomView const& root, std::string_view targetSampleEntryType)
  {
    auto rootCursor = root.children();

    while (true)
    {
      auto movieResult = rootCursor.next();

      if (!movieResult)
      {
        return std::unexpected{movieResult.error()};
      }

      if (!*movieResult)
      {
        return makeError(Error::Code::NotFound, "MP4 audio track was not found");
      }

      auto const& movie = **movieResult;

      if (movie.type() != "moov")
      {
        continue;
      }

      auto selectionResult = findAudioTrackInMovie(movie, targetSampleEntryType);

      if (selectionResult)
      {
        return selectionResult;
      }

      if (selectionResult.error().code != Error::Code::NotFound)
      {
        return std::unexpected{selectionResult.error()};
      }
    }
  }
} // namespace ao::media::mp4
