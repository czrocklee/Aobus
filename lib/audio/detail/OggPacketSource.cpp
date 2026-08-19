// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OggPacketSource.h"

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/media/ogg/Demuxer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <utility>

namespace ao::audio::detail
{
  Result<> OggPacketSource::open(std::filesystem::path const& filePath)
  {
    close();

    if (auto const result = _mappedFile.map(filePath); !result)
    {
      return std::unexpected{result.error()};
    }

    auto demuxerRes = media::ogg::Demuxer::parse(_mappedFile.bytes());

    if (!demuxerRes)
    {
      auto error = demuxerRes.error();
      close();
      return std::unexpected{std::move(error)};
    }

    _optDemuxer.emplace(std::move(*demuxerRes));
    _packetIndex = 0;
    return {};
  }

  void OggPacketSource::close() noexcept
  {
    _optDemuxer.reset();
    _mappedFile.unmap();
    _packetIndex = 0;
  }

  std::int64_t OggPacketSource::seekToGranule(std::int64_t granulePosition) noexcept
  {
    if (!_optDemuxer)
    {
      return 0;
    }

    auto const restart = _optDemuxer->restartAtGranule(granulePosition);
    _packetIndex = restart.packetIndex;
    return std::max<std::int64_t>(restart.granulePosition, 0);
  }

  bool OggPacketSource::isOpen() const noexcept
  {
    return _optDemuxer.has_value();
  }

  bool OggPacketSource::isAtEnd() const noexcept
  {
    return !_optDemuxer || _packetIndex >= _optDemuxer->packetCount();
  }

  std::span<std::byte const> OggPacketSource::packet() const
  {
    if (isAtEnd())
    {
      return {};
    }

    AO_INVARIANT(_optDemuxer, "Readable Ogg packet source has no demuxer");
    return _optDemuxer->packet(_packetIndex).bytes;
  }

  std::span<std::byte const> OggPacketSource::packetAt(std::size_t index) const
  {
    if (!_optDemuxer || index >= _optDemuxer->packetCount())
    {
      return {};
    }

    return _optDemuxer->packet(index).bytes;
  }

  std::size_t OggPacketSource::packetIndex() const noexcept
  {
    return _packetIndex;
  }

  std::size_t OggPacketSource::packetCount() const noexcept
  {
    return _optDemuxer ? _optDemuxer->packetCount() : 0;
  }

  std::int64_t OggPacketSource::finalGranulePosition() const noexcept
  {
    return _optDemuxer ? _optDemuxer->finalGranulePosition() : media::ogg::Demuxer::kUnsetGranulePosition;
  }

  media::ogg::Demuxer const& OggPacketSource::demuxer() const noexcept
  {
    AO_EXPECTS(_optDemuxer);
    return *_optDemuxer;
  }

  void OggPacketSource::setPacketIndex(std::size_t index) noexcept
  {
    _packetIndex = std::min(index, packetCount());
  }

  void OggPacketSource::advance() noexcept
  {
    if (!isAtEnd())
    {
      ++_packetIndex;
    }
  }
} // namespace ao::audio::detail
