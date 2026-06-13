// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Mp4PacketSource.h"

#include "TimeConversion.h"
#include <ao/Error.h>
#include <ao/audio/Types.h>
#include <ao/media/mp4/Demuxer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace ao::audio::detail
{
  Result<> Mp4PacketSource::open(std::filesystem::path const& filePath, std::string_view sampleEntry)
  {
    close();

    if (auto const result = _mappedFile.map(filePath); !result)
    {
      return std::unexpected{result.error()};
    }

    _demuxerPtr = std::make_unique<media::mp4::Demuxer>(_mappedFile.bytes());

    if (auto const result = _demuxerPtr->parseTrack(sampleEntry); !result)
    {
      auto const& error = result.error();
      close();
      return std::unexpected{error};
    }

    _sampleIndex = 0;
    return {};
  }

  void Mp4PacketSource::close() noexcept
  {
    _demuxerPtr.reset();
    _mappedFile.unmap();
    _sampleIndex = 0;
  }

  Result<> Mp4PacketSource::seek(std::chrono::milliseconds offset, std::uint32_t fallbackTimescale)
  {
    if (!_demuxerPtr)
    {
      return makeError(Error::Code::SeekFailed, "MP4 packet source is not open");
    }

    auto const effectiveTimescale = timescale(fallbackTimescale);

    if (effectiveTimescale == 0)
    {
      return makeError(Error::Code::SeekFailed, "Timescale is 0");
    }

    auto const targetTime = durationToSamples(offset, effectiveTimescale);
    _sampleIndex = _demuxerPtr->sampleIndexAtTime(targetTime);
    return {};
  }

  bool Mp4PacketSource::isOpen() const noexcept
  {
    return _demuxerPtr != nullptr;
  }

  bool Mp4PacketSource::atEnd() const noexcept
  {
    return !_demuxerPtr || _sampleIndex >= _demuxerPtr->sampleCount();
  }

  std::span<std::byte const> Mp4PacketSource::packet() const
  {
    return atEnd() ? std::span<std::byte const>{} : _demuxerPtr->samplePayload(_sampleIndex);
  }

  std::span<std::byte const> Mp4PacketSource::magicCookie() const
  {
    return _demuxerPtr ? _demuxerPtr->magicCookie() : std::span<std::byte const>{};
  }

  media::mp4::Demuxer::SampleEntry Mp4PacketSource::sampleInfo() const
  {
    return atEnd() ? media::mp4::Demuxer::SampleEntry{} : _demuxerPtr->sampleInfo(_sampleIndex);
  }

  std::uint32_t Mp4PacketSource::sampleIndex() const noexcept
  {
    return _sampleIndex;
  }

  std::uint32_t Mp4PacketSource::timescale(std::uint32_t fallback) const noexcept
  {
    if (_demuxerPtr && _demuxerPtr->timescale() > 0)
    {
      return _demuxerPtr->timescale();
    }

    return fallback;
  }

  std::chrono::milliseconds Mp4PacketSource::duration(std::uint32_t fallbackTimescale) const noexcept
  {
    auto const effectiveTimescale = timescale(fallbackTimescale);

    if (!_demuxerPtr || effectiveTimescale == 0)
    {
      return std::chrono::milliseconds{0};
    }

    return convertToDuration(_demuxerPtr->duration(), effectiveTimescale);
  }

  std::uint64_t Mp4PacketSource::firstFrameIndex(std::uint32_t sampleRate,
                                                 std::uint32_t fallbackFramesPerPacket) const noexcept
  {
    if (atEnd())
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
    if (!atEnd())
    {
      ++_sampleIndex;
    }
  }
} // namespace ao::audio::detail
