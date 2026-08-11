// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Mp4PacketSource.h"

#include "AudioTime.h"
#include "TimeConversion.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/media/mp4/Demuxer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace ao::audio::detail
{
  Result<> Mp4PacketSource::open(std::filesystem::path const& filePath, std::string_view sampleEntry)
  {
    close();

    if (auto const result = _mappedFile.map(filePath); !result)
    {
      return std::unexpected{result.error()};
    }

    auto demuxerRes = media::mp4::Demuxer::parse(_mappedFile.bytes(), sampleEntry);

    if (!demuxerRes)
    {
      auto error = demuxerRes.error();
      close();
      return std::unexpected{std::move(error)};
    }

    _optDemuxer.emplace(std::move(*demuxerRes));
    _sampleIndex = 0;
    return {};
  }

  void Mp4PacketSource::close() noexcept
  {
    _optDemuxer.reset();
    _mappedFile.unmap();
    _sampleIndex = 0;
  }

  Result<> Mp4PacketSource::seek(std::chrono::milliseconds offset, std::uint32_t fallbackTimescale)
  {
    if (!_optDemuxer)
    {
      return makeError(Error::Code::SeekFailed, "MP4 packet source is not open");
    }

    auto const effectiveTimescale = timescale(fallbackTimescale);

    if (effectiveTimescale == 0)
    {
      return makeError(Error::Code::SeekFailed, "Timescale is 0");
    }

    auto const targetTime = durationToSamples(offset, effectiveTimescale);
    _sampleIndex = _optDemuxer->sampleIndexAtTime(targetTime);
    return {};
  }

  bool Mp4PacketSource::isOpen() const noexcept
  {
    return _optDemuxer.has_value();
  }

  bool Mp4PacketSource::isAtEnd() const noexcept
  {
    return !_optDemuxer || _sampleIndex >= _optDemuxer->sampleCount();
  }

  std::span<std::byte const> Mp4PacketSource::packet() const
  {
    if (isAtEnd())
    {
      return {};
    }

    AO_INVARIANT(_optDemuxer, "Readable MP4 packet source has no demuxer");
    return _optDemuxer->samplePayload(_sampleIndex);
  }

  std::span<std::byte const> Mp4PacketSource::magicCookie() const
  {
    return _optDemuxer ? _optDemuxer->magicCookie() : std::span<std::byte const>{};
  }

  media::mp4::Demuxer::SampleEntry Mp4PacketSource::sampleInfo() const
  {
    if (isAtEnd())
    {
      return {};
    }

    AO_INVARIANT(_optDemuxer, "Readable MP4 packet source has no demuxer");
    return _optDemuxer->sampleInfo(_sampleIndex);
  }

  std::uint32_t Mp4PacketSource::sampleIndex() const noexcept
  {
    return _sampleIndex;
  }

  std::uint32_t Mp4PacketSource::timescale(std::uint32_t fallback) const noexcept
  {
    if (_optDemuxer && _optDemuxer->timescale() > 0)
    {
      return _optDemuxer->timescale();
    }

    return fallback;
  }

  std::chrono::milliseconds Mp4PacketSource::duration(std::uint32_t fallbackTimescale) const noexcept
  {
    auto const effectiveTimescale = timescale(fallbackTimescale);

    if (!_optDemuxer || effectiveTimescale == 0)
    {
      return std::chrono::milliseconds{0};
    }

    return convertToDuration(_optDemuxer->duration(), effectiveTimescale);
  }

  std::uint64_t Mp4PacketSource::firstFrameIndex(std::uint32_t sampleRate,
                                                 std::uint32_t fallbackFramesPerPacket) const noexcept
  {
    if (isAtEnd())
    {
      return 0;
    }

    auto const entry = sampleInfo();

    if (auto const mediaTimescale = timescale();
        mediaTimescale > 0 && sampleRate > 0 && (entry.startTime > 0 || entry.duration > 0))
    {
      return saturatingScale(entry.startTime, sampleRate, mediaTimescale);
    }

    return static_cast<std::uint64_t>(_sampleIndex) * fallbackFramesPerPacket;
  }

  void Mp4PacketSource::advance() noexcept
  {
    if (!isAtEnd())
    {
      ++_sampleIndex;
    }
  }
} // namespace ao::audio::detail
