// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/TrackSelection.h>

#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
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
      auto nodeRes = findAtom(track, kTrackHdlrPath);

      if (!nodeRes)
      {
        return std::unexpected{nodeRes.error()};
      }

      if (!*nodeRes)
      {
        return makeError(Error::Code::NotFound, "MP4 track has no handler atom");
      }

      auto const payload = (*nodeRes)->payload();

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
      auto entryRes = cursor.next();

      if (!entryRes)
      {
        return std::unexpected{entryRes.error()};
      }

      if (!*entryRes)
      {
        return makeError(Error::Code::NotFound, "MP4 sample description entry is missing");
      }

      return std::string{(**entryRes).type()};
    }

    bool isSupportedAudioSampleEntry(std::string_view sampleEntryType) noexcept
    {
      return sampleEntryType == "alac" || sampleEntryType == "mp4a";
    }

    Result<AudioTrackSelection> selectTrack(AtomView const& track, std::string_view targetSampleEntryType)
    {
      auto stsdRes = findAtom(track, kTrackStsdPath);

      if (!stsdRes)
      {
        return std::unexpected{stsdRes.error()};
      }

      if (!*stsdRes)
      {
        return makeError(Error::Code::NotFound, "MP4 track has no sample description");
      }

      auto sampleEntryTypeRes = firstSampleEntryType(**stsdRes);

      if (!sampleEntryTypeRes)
      {
        return std::unexpected{sampleEntryTypeRes.error()};
      }

      auto sampleEntryType = std::move(*sampleEntryTypeRes);

      if (!targetSampleEntryType.empty() && sampleEntryType != targetSampleEntryType)
      {
        return makeError(Error::Code::NotFound, "MP4 track sample entry does not match");
      }

      if (auto handlerTypeRes = trackHandlerType(track); !handlerTypeRes)
      {
        if (handlerTypeRes.error().code != Error::Code::NotFound)
        {
          return std::unexpected{handlerTypeRes.error()};
        }

        if (!isSupportedAudioSampleEntry(sampleEntryType))
        {
          return makeError(Error::Code::NotFound, "MP4 track is not recognizable as audio");
        }
      }
      else if (*handlerTypeRes != "soun")
      {
        return makeError(Error::Code::NotFound, "MP4 track handler is not audio");
      }

      return AudioTrackSelection{
        .track = track,
        .stsd = **stsdRes,
        .sampleEntryType = std::move(sampleEntryType),
      };
    }

    Result<AudioTrackSelection> findAudioTrackInMovie(AtomView const& movie, std::string_view targetSampleEntryType)
    {
      auto trackCursor = movie.children();

      while (true)
      {
        auto trackRes = trackCursor.next();

        if (!trackRes)
        {
          return std::unexpected{trackRes.error()};
        }

        if (!*trackRes)
        {
          return makeError(Error::Code::NotFound, "MP4 movie has no matching audio track");
        }

        auto const& track = **trackRes;

        if (track.type() != "trak")
        {
          continue;
        }

        auto selectionRes = selectTrack(track, targetSampleEntryType);

        if (selectionRes)
        {
          return selectionRes;
        }

        if (selectionRes.error().code != Error::Code::NotFound)
        {
          return std::unexpected{selectionRes.error()};
        }
      }
    }
  } // namespace

  Result<AudioTrackSelection> findAudioTrack(AtomView const& root, std::string_view targetSampleEntryType)
  {
    auto rootCursor = root.children();

    while (true)
    {
      auto movieRes = rootCursor.next();

      if (!movieRes)
      {
        return std::unexpected{movieRes.error()};
      }

      if (!*movieRes)
      {
        return makeError(Error::Code::NotFound, "MP4 audio track was not found");
      }

      auto const& movie = **movieRes;

      if (movie.type() != "moov")
      {
        continue;
      }

      auto selectionRes = findAudioTrackInMovie(movie, targetSampleEntryType);

      if (selectionRes)
      {
        return selectionRes;
      }

      if (selectionRes.error().code != Error::Code::NotFound)
      {
        return std::unexpected{selectionRes.error()};
      }
    }
  }
} // namespace ao::media::mp4
