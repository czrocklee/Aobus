// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/SegmentDecoderSession.h>

#include "detail/TimeConversion.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace ao::audio
{
  namespace
  {
    class [[nodiscard]] SegmentDecoderSession final : public DecoderSession
    {
    public:
      SegmentDecoderSession(std::unique_ptr<DecoderSession> decoderPtr,
                            PlaybackSegment const& segment,
                            DecodedStreamInfo const& info) noexcept
        : _decoderPtr{std::move(decoderPtr)}, _segment{segment}, _info{info}
      {
        _info.optTotalFrames = segment.frameCount;
        auto const durationMs = detail::saturatingScale(segment.frameCount, 1000, info.sourceFormat.sampleRate);
        _info.duration = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
          std::min<std::uint64_t>(durationMs, std::numeric_limits<std::chrono::milliseconds::rep>::max()))};
      }

      Result<> seek(std::chrono::milliseconds offset) noexcept override
      {
        if (offset.count() < 0)
        {
          return makeError(Error::Code::SeekFailed, "Negative segment seek offset");
        }

        return seekFrame(
          detail::saturatingScale(static_cast<std::uint64_t>(offset.count()), _info.sourceFormat.sampleRate, 1000));
      }

      bool supportsFrameSeek() const noexcept override { return true; }

      Result<> seekFrame(std::uint64_t frameIndex) noexcept override
      {
        if (frameIndex >= _segment.frameCount)
        {
          _nextFrame = _segment.frameCount;
          _optError.reset();
          return {};
        }

        auto seekRes = _decoderPtr->seekFrame(_segment.startFrame + frameIndex);

        if (!seekRes)
        {
          return seekRes;
        }

        _nextFrame = frameIndex;
        _optError.reset();
        return {};
      }

      void flush() noexcept override { _decoderPtr->flush(); }

      Result<PcmBlock> readNextBlock() noexcept override
      {
        if (_optError)
        {
          return std::unexpected{*_optError};
        }

        if (_nextFrame == _segment.frameCount)
        {
          return PcmBlock{.firstFrameIndex = _nextFrame, .endOfStream = true};
        }

        auto const bytesPerFrame = frameBytes(_info.outputFormat);
        auto const target = _segment.startFrame + _nextFrame;

        while (true)
        {
          auto blockRes = _decoderPtr->readNextBlock();

          if (!blockRes)
          {
            return std::unexpected{blockRes.error()};
          }

          auto const& block = *blockRes;

          if (block.frames == 0 || block.firstFrameIndex > target ||
              block.bytes.size() / bytesPerFrame != block.frames || block.bytes.size() % bytesPerFrame != 0 ||
              block.firstFrameIndex > std::numeric_limits<std::uint64_t>::max() - block.frames)
          {
            return failDecode();
          }

          // A frame seek may position before its target; discard only frames
          // whose declared coordinates precede it, never relabel later PCM.
          if (auto const blockEnd = block.firstFrameIndex + block.frames; blockEnd <= target)
          {
            if (block.endOfStream)
            {
              return failDecode();
            }

            continue;
          }

          auto const skipFrames = target - block.firstFrameIndex;
          auto const frames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(block.frames - skipFrames, _segment.frameCount - _nextFrame));
          auto const firstFrame = _nextFrame;
          _nextFrame += frames;

          if (block.endOfStream && _nextFrame < _segment.frameCount)
          {
            return failDecode();
          }

          return PcmBlock{
            .bytes = block.bytes.subspan(
              static_cast<std::size_t>(skipFrames) * bytesPerFrame, static_cast<std::size_t>(frames) * bytesPerFrame),
            .frames = frames,
            .firstFrameIndex = firstFrame,
            .endOfStream = _nextFrame == _segment.frameCount,
          };
        }
      }

      DecodedStreamInfo streamInfo() const noexcept override { return _info; }

    private:
      Result<PcmBlock> failDecode() noexcept
      {
        _optError =
          Error{.code = Error::Code::DecodeFailed, .message = "Carrier ended early or returned invalid segment PCM"};
        return std::unexpected{*_optError};
      }

      std::unique_ptr<DecoderSession> _decoderPtr;
      PlaybackSegment _segment;
      DecodedStreamInfo _info;
      std::uint64_t _nextFrame = 0;
      std::optional<Error> _optError{};
    };
  } // namespace

  Result<std::unique_ptr<DecoderSession>> makeSegmentDecoderSession(std::unique_ptr<DecoderSession> decoderPtr,
                                                                    PlaybackSegment const& segment,
                                                                    std::uint32_t expectedRate) noexcept
  {
    if (!decoderPtr)
    {
      return makeError(Error::Code::InvalidInput, "Segment decoder is absent");
    }

    auto const info = decoderPtr->streamInfo();

    if (!decoderPtr->supportsFrameSeek() || !info.optTotalFrames)
    {
      return makeError(Error::Code::NotSupported, "Segment requires an exact extent and exact frame seeking");
    }

    if (expectedRate == 0 || info.sourceFormat.sampleRate != expectedRate ||
        *info.optTotalFrames != segment.totalFrames || segment.frameCount == 0 ||
        segment.startFrame > segment.totalFrames || segment.frameCount > segment.totalFrames - segment.startFrame ||
        info.outputFormat.sampleRate != expectedRate || frameBytes(info.outputFormat) == 0)
    {
      return makeError(Error::Code::FormatRejected, "Segment does not match the carrier rate, extent, or PCM format");
    }

    auto segmentPtr = std::make_unique<SegmentDecoderSession>(std::move(decoderPtr), segment, info);

    if (auto const seekRes = segmentPtr->seekFrame(0); !seekRes)
    {
      return std::unexpected{seekRes.error()};
    }

    return std::unique_ptr<DecoderSession>{std::move(segmentPtr)};
  }
} // namespace ao::audio
