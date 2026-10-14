// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "FlacDecoderSession.h"

#include "AudioTime.h"
#include "PcmConversion.h"
#include "detail/DecoderError.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/MappedFileCursor.h"
#include "detail/TimeConversion.h"
#include <ao/AudioCodec.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/ByteView.h>

#include <FLAC/format.h>
#include <FLAC/ordinals.h>
#include <FLAC/stream_decoder.h>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kLowByteMask = 0xFF;

    struct FlacFrameFormatMismatch final
    {
      SignalFormat expectedFormat;
      std::uint32_t frameSampleRate;
      std::uint32_t frameChannels;
      std::uint32_t framePrecisionBits;
      std::source_location location;
    };

    struct FlacFrameDiscontinuity final
    {
      std::uint64_t expectedFrameIndex;
      std::uint64_t frameIndex;
      std::uint32_t pendingFrames;
      std::source_location location;
    };

    template<typename Callback>
    decltype(auto) invokeFlacCallback(std::string_view const context,
                                      Callback&& callback,
                                      std::source_location const location = std::source_location::current()) noexcept
    {
      try
      {
        return std::forward<Callback>(callback)();
      }
      catch (...)
      {
        fatalFromException(std::current_exception(), context, location);
      }
    }

    SampleEncoding nativeFlacEncoding(std::uint8_t const precisionBits) noexcept
    {
      if (precisionBits <= 16U)
      {
        return SampleEncoding::Signed16Le;
      }

      if (precisionBits <= 24U)
      {
        return SampleEncoding::Signed24PackedLe;
      }

      return SampleEncoding::Signed32Le;
    }

    ::FLAC__StreamMetadata_StreamInfo const& streamInfoFor(::FLAC__StreamMetadata const* metadata)
    {
      // libFLAC exposes stream-specific metadata through this tagged C union.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      return metadata->data.stream_info;
    }

    std::uint64_t frameSampleNumberFor(::FLAC__Frame const* frame) noexcept
    {
      // libFLAC normalizes fixed-block frame numbers to sample numbers before
      // delivery, including the trimmed header of a partial seek-target block.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      return frame->header.number.sample_number;
    }

    std::int32_t alignSample(std::int32_t sample, std::uint8_t sourceBitDepth, std::uint8_t targetBitDepth) noexcept
    {
      if (sourceBitDepth > targetBitDepth)
      {
        return sample >> (sourceBitDepth - targetBitDepth);
      }

      if (sourceBitDepth < targetBitDepth)
      {
        auto bits = static_cast<std::uint32_t>(sample);
        bits <<= targetBitDepth - sourceBitDepth;
        return std::bit_cast<std::int32_t>(bits);
      }

      return sample;
    }
  } // namespace

  struct FlacDecoderSession::Impl final
  {
    detail::DecoderOutputAdapter outputAdapter;
    ::FLAC__StreamDecoder* decoder = nullptr;

    detail::MappedFileCursor fileCursor;

    DecodedStreamInfo info;

    // Buffer for decoded samples
    std::vector<std::byte> pcmBuffer;
    std::span<std::byte const> outputBytes{};
    // Reused per-channel span scratch for interleaving (avoids a per-block heap
    // allocation in the write callback). Capacity persists across blocks.
    std::vector<std::span<std::int32_t const>> channelSpans;
    std::uint32_t bufferedFrames = 0;
    std::uint64_t nextFrameIndex = 0;
    std::uint64_t totalFrames = 0;
    std::optional<::FLAC__StreamDecoderErrorStatus> optDecodeError;
    std::optional<FlacFrameFormatMismatch> optFormatMismatch;
    std::optional<FlacFrameDiscontinuity> optFrameDiscontinuity;
    std::optional<Error> optOutputError;
    // Only seek_absolute's synchronous write callback uses this target; normal
    // delivery is checked against the next public block's sequential index.
    std::optional<std::uint64_t> optSeekTarget;
    bool hasConfiguredStreamInfo = false;
    bool eof = false;

    Impl(std::optional<SampleEncoding> optOutputEncoding)
      : outputAdapter{optOutputEncoding}, decoder{::FLAC__stream_decoder_new()}
    {
    }

    ~Impl()
    {
      if (decoder != nullptr)
      {
        ::FLAC__stream_decoder_delete(decoder);
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // All callbacks are noexcept; allocating write/metadata paths also use AO
    // exception-aware fatal containment before returning through libFLAC.
    static ::FLAC__StreamDecoderReadStatus readCallback(::FLAC__StreamDecoder const* decoder,
                                                        ::FLAC__byte* buffer,
                                                        std::size_t* bytes,
                                                        void* clientData) noexcept;

    static ::FLAC__StreamDecoderSeekStatus seekCallback(::FLAC__StreamDecoder const* decoder,
                                                        ::FLAC__uint64 absoluteByteOffset,
                                                        void* clientData) noexcept;

    static ::FLAC__StreamDecoderTellStatus tellCallback(::FLAC__StreamDecoder const* decoder,
                                                        ::FLAC__uint64* absoluteByteOffset,
                                                        void* clientData) noexcept;

    static ::FLAC__StreamDecoderLengthStatus lengthCallback(::FLAC__StreamDecoder const* decoder,
                                                            ::FLAC__uint64* streamLength,
                                                            void* clientData) noexcept;

    static ::FLAC__bool eofCallback(::FLAC__StreamDecoder const* decoder, void* clientData) noexcept;

    static ::FLAC__StreamDecoderWriteStatus writeCallback(::FLAC__StreamDecoder const* decoder,
                                                          ::FLAC__Frame const* frame,
                                                          ::FLAC__int32 const* const* buffer,
                                                          void* clientData) noexcept;

    static void metadataCallback(::FLAC__StreamDecoder const* decoder,
                                 ::FLAC__StreamMetadata const* metadata,
                                 void* clientData) noexcept;

    static void errorCallback(::FLAC__StreamDecoder const* decoder,
                              ::FLAC__StreamDecoderErrorStatus status,
                              void* clientData) noexcept;

    void checkDecodeError() const
    {
      // The session-detected frame-format failure is the direct cause of an
      // aborted decode, so it wins over the more generic libFLAC error status.
      if (optFormatMismatch)
      {
        auto const& mismatch = *optFormatMismatch;
        // Materialize the owning diagnostic only after returning from libFLAC.
        detail::throwDecoderError(
          Error::Code::DecodeFailed,
          std::format("FLAC frame does not match the stream format: expected {} Hz, {} channels, "
                      "{} bits per sample, frame declares {} Hz, {} channels, {} bits per sample",
                      mismatch.expectedFormat.sampleRate,
                      mismatch.expectedFormat.channels,
                      mismatch.expectedFormat.precisionBits,
                      mismatch.frameSampleRate,
                      mismatch.frameChannels,
                      mismatch.framePrecisionBits),
          mismatch.location);
      }

      if (optFrameDiscontinuity)
      {
        auto const& discontinuity = *optFrameDiscontinuity;
        detail::throwDecoderError(Error::Code::DecodeFailed,
                                  std::format("FLAC frame does not continue the decoded timeline: expected sample {}, "
                                              "frame declares sample {}, pending frames {}",
                                              discontinuity.expectedFrameIndex,
                                              discontinuity.frameIndex,
                                              discontinuity.pendingFrames),
                                  discontinuity.location);
      }

      if (optDecodeError)
      {
        detail::throwDecoderError(
          Error::Code::DecodeFailed,
          std::string{"FLAC decode error: "} + ::FLAC__StreamDecoderErrorStatusString[*optDecodeError]);
      }

      if (optOutputError)
      {
        detail::throwDecoderError(*optOutputError);
      }
    }

    void checkEndOfStream() const
    {
      if (totalFrames != 0 && nextFrameIndex + bufferedFrames < totalFrames)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "FLAC stream ended before the declared sample count");
      }
    }
  };

  FlacDecoderSession::FlacDecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
  {
  }

  FlacDecoderSession::~FlacDecoderSession() = default;

  Result<> FlacDecoderSession::initialize(std::filesystem::path const& filePath) noexcept
  {
    try
    {
      if (auto const res = _implPtr->fileCursor.open(filePath); !res)
      {
        detail::throwDecoderError(res.error());
      }

      _implPtr->eof = false;
      _implPtr->nextFrameIndex = 0;

      auto const initStatus = ::FLAC__stream_decoder_init_stream(_implPtr->decoder,
                                                                 Impl::readCallback,
                                                                 Impl::seekCallback,
                                                                 Impl::tellCallback,
                                                                 Impl::lengthCallback,
                                                                 Impl::eofCallback,
                                                                 Impl::writeCallback,
                                                                 Impl::metadataCallback,
                                                                 Impl::errorCallback,
                                                                 _implPtr.get());

      if (initStatus != ::FLAC__STREAM_DECODER_INIT_STATUS_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to initialize FLAC decoder");
      }

      // Process until metadata is read
      if (::FLAC__stream_decoder_process_until_end_of_metadata(_implPtr->decoder) == 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to read FLAC metadata");
      }

      _implPtr->checkDecodeError();

      if (auto const& sourceFormat = _implPtr->info.sourceFormat;
          !_implPtr->hasConfiguredStreamInfo || sourceFormat.sampleRate == 0 || sourceFormat.channels == 0 ||
          sourceFormat.channels > FLAC__MAX_CHANNELS || sourceFormat.precisionBits < FLAC__MIN_BITS_PER_SAMPLE ||
          sourceFormat.precisionBits > FLAC__MAX_BITS_PER_SAMPLE)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "FLAC stream is missing valid STREAMINFO");
      }

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  // Result error materialization may allocate; DecoderSession requires fail-fast
  // termination if that escapes its recoverable decoder-error boundary.
  Result<> FlacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    try
    {
      // libFLAC cannot seek out of these states until explicitly flushed.
      // Keep the recorded cause intact when this seek cannot restart decoding.
      if (auto const decoderState = ::FLAC__stream_decoder_get_state(_implPtr->decoder);
          decoderState == ::FLAC__STREAM_DECODER_ABORTED || decoderState == ::FLAC__STREAM_DECODER_SEEK_ERROR)
      {
        _implPtr->checkDecodeError();

        detail::throwDecoderError(Error::Code::SeekFailed, "FLAC decoder must be flushed before seeking again");
      }

      _implPtr->pcmBuffer.clear();
      _implPtr->outputBytes = {};
      _implPtr->bufferedFrames = 0;
      _implPtr->optDecodeError.reset();
      _implPtr->optFormatMismatch.reset();
      _implPtr->optFrameDiscontinuity.reset();
      _implPtr->optOutputError.reset();
      _implPtr->eof = false;

      auto const sampleRate = _implPtr->info.sourceFormat.sampleRate;

      if (sampleRate == 0)
      {
        detail::throwDecoderError(Error::Code::SeekFailed, "Sample rate is 0");
      }

      auto const requestedSample = durationToSamples(offset, sampleRate);

      // total_samples is one past the last valid sample index. Seeking libFLAC at
      // or beyond it is out of range, and builds disagree on the outcome: some
      // position at end of stream and immediately drain the source, dropping the
      // committed seek position. Clamp to the last decodable sample so a seek to
      // the reported duration lands on the final frame instead of past the end.
      auto const totalFrames = _implPtr->totalFrames;
      auto const targetSample = static_cast<::FLAC__uint64>(
        (totalFrames != 0 && requestedSample >= totalFrames) ? totalFrames - 1 : requestedSample);

      // libFLAC delivers the seek target before seek_absolute returns and may
      // trim it to a partial block. Validate that delivery against the target,
      // not the sequential position from before the seek.
      _implPtr->optSeekTarget = targetSample;
      auto const sought = ::FLAC__stream_decoder_seek_absolute(_implPtr->decoder, targetSample);
      _implPtr->optSeekTarget.reset();

      if (sought == 0)
      {
        // A seek that aborted inside a callback stored the concrete failure;
        // surface it instead of masking it with the generic seek failure.
        _implPtr->checkDecodeError();

        detail::throwDecoderError(Error::Code::SeekFailed, "FLAC seek failed");
      }

      _implPtr->checkDecodeError();

      _implPtr->nextFrameIndex = targetSample;

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  void FlacDecoderSession::flush() noexcept
  {
    if (_implPtr->fileCursor.isOpen())
    {
      auto const decoderState = ::FLAC__stream_decoder_get_state(_implPtr->decoder);
      auto const preservePosition = !_implPtr->optDecodeError && !_implPtr->optFormatMismatch &&
                                    !_implPtr->optFrameDiscontinuity && !_implPtr->optOutputError &&
                                    (decoderState == ::FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC ||
                                     decoderState == ::FLAC__STREAM_DECODER_END_OF_STREAM);
      ::FLAC__uint64 bytePosition = 0;
      auto const hasPosition =
        preservePosition && ::FLAC__stream_decoder_get_decode_position(_implPtr->decoder, &bytePosition) != 0;
      auto const flushed = ::FLAC__stream_decoder_flush(_implPtr->decoder);

      if (preservePosition)
      {
        // The provider cursor includes read-ahead bytes. Capture the decoded
        // boundary before flushing, then rewind the cursor and retain any
        // already decoded but unread seek-target PCM at its original index.
        if (hasPosition && flushed != 0 &&
            bytePosition <= static_cast<::FLAC__uint64>(std::numeric_limits<std::int64_t>::max()) &&
            _implPtr->fileCursor.seek(static_cast<std::int64_t>(bytePosition), detail::SeekOrigin::Begin))
        {
          return;
        }

        _implPtr->optOutputError = Error{
          .code = Error::Code::DecodeFailed,
          .message = "FLAC flush could not preserve the decode position",
          .location = std::source_location::current(),
        };
      }
    }

    _implPtr->pcmBuffer.clear();
    _implPtr->outputBytes = {};
    _implPtr->bufferedFrames = 0;
  }

  // Result error materialization may allocate; DecoderSession requires fail-fast
  // termination if that escapes its recoverable decoder-error boundary.
  Result<PcmBlock> FlacDecoderSession::readNextBlock() noexcept
  {
    try
    {
      if (!_implPtr->fileCursor.isOpen())
      {
        return PcmBlock{.endOfStream = true};
      }

      _implPtr->checkDecodeError();

      if (_implPtr->eof && _implPtr->bufferedFrames == 0)
      {
        return PcmBlock{.endOfStream = true};
      }

      // If buffer is empty, process a single frame
      while (_implPtr->bufferedFrames == 0 && !_implPtr->eof)
      {
        if (::FLAC__stream_decoder_process_single(_implPtr->decoder) == 0)
        {
          // An aborted callback stored the concrete failure; surface it instead
          // of masking it with the generic process failure below.
          _implPtr->checkDecodeError();

          if (::FLAC__stream_decoder_get_state(_implPtr->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM)
          {
            _implPtr->checkEndOfStream();

            _implPtr->eof = true;
            break;
          }

          detail::throwDecoderError(Error::Code::DecodeFailed, "FLAC process single failed");
        }

        _implPtr->checkDecodeError();

        // process_single can report success in a failed state without producing
        // a frame; do not retry indefinitely when no stored callback cause exists.
        auto const decoderState = ::FLAC__stream_decoder_get_state(_implPtr->decoder);

        if (decoderState == ::FLAC__STREAM_DECODER_ABORTED)
        {
          detail::throwDecoderError(Error::Code::DecodeFailed, "FLAC decoder cannot continue from its failed state");
        }

        if (decoderState == ::FLAC__STREAM_DECODER_END_OF_STREAM)
        {
          _implPtr->checkEndOfStream();

          _implPtr->eof = true;
          break;
        }
      }

      if (_implPtr->bufferedFrames == 0 && _implPtr->eof)
      {
        return PcmBlock{.endOfStream = true};
      }

      auto block = PcmBlock{
        .bytes = _implPtr->outputBytes,
        .frames = _implPtr->bufferedFrames,
        .firstFrameIndex = _implPtr->nextFrameIndex,
        .endOfStream = _implPtr->eof &&
                       (::FLAC__stream_decoder_get_state(_implPtr->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM),
      };

      _implPtr->nextFrameIndex += _implPtr->bufferedFrames;
      _implPtr->bufferedFrames = 0;

      return block;
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  DecodedStreamInfo FlacDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }

  // Implementation of callbacks

  ::FLAC__StreamDecoderReadStatus FlacDecoderSession::Impl::readCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                         ::FLAC__byte* buffer,
                                                                         std::size_t* bytes,
                                                                         void* clientData) noexcept
  {
    if (auto* const impl = utility::unsafeDowncast<Impl>(clientData); *bytes > 0)
    {
      auto const count = impl->fileCursor.read(utility::bytes::view(static_cast<void*>(buffer), *bytes));
      *bytes = count;

      if (count == 0)
      {
        return ::FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
      }

      return ::FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    return ::FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  ::FLAC__StreamDecoderSeekStatus FlacDecoderSession::Impl::seekCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                         ::FLAC__uint64 absoluteByteOffset,
                                                                         void* clientData) noexcept
  {
    if (auto* const impl = utility::unsafeDowncast<Impl>(clientData);
        absoluteByteOffset > static_cast<::FLAC__uint64>(std::numeric_limits<std::int64_t>::max()) ||
        !impl->fileCursor.seek(static_cast<std::int64_t>(absoluteByteOffset), detail::SeekOrigin::Begin))
    {
      return ::FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }

    return ::FLAC__STREAM_DECODER_SEEK_STATUS_OK;
  }

  ::FLAC__StreamDecoderTellStatus FlacDecoderSession::Impl::tellCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                         ::FLAC__uint64* absoluteByteOffset,
                                                                         void* clientData) noexcept
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    *absoluteByteOffset = impl->fileCursor.position();

    return ::FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  ::FLAC__StreamDecoderLengthStatus FlacDecoderSession::Impl::lengthCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                             ::FLAC__uint64* streamLength,
                                                                             void* clientData) noexcept
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    *streamLength = impl->fileCursor.size();

    return ::FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }

  ::FLAC__bool FlacDecoderSession::Impl::eofCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                     void* clientData) noexcept
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);

    return static_cast<::FLAC__bool>(impl->fileCursor.isAtEnd());
  }

  ::FLAC__StreamDecoderWriteStatus FlacDecoderSession::Impl::writeCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                           ::FLAC__Frame const* frame,
                                                                           ::FLAC__int32 const* const* buffer,
                                                                           void* clientData) noexcept
  {
    return invokeFlacCallback(
      "FLAC write callback",
      [&]
      {
        auto* const impl = utility::unsafeDowncast<Impl>(clientData);

        // libFLAC accepts frames whose header contradicts the STREAMINFO format
        // instead of rejecting them, so a frame may arrive with a sample rate,
        // channel count, or bit depth different from the established source
        // format. Copying, resizing, interleaving, or converting such a frame
        // would emit PCM that contradicts the published session format, so guard
        // the frame before any buffer work and abort without emitting its PCM.
        if (auto const& sourceFormat = impl->info.sourceFormat;
            frame->header.sample_rate != sourceFormat.sampleRate ||
            frame->header.channels != static_cast<std::uint32_t>(sourceFormat.channels) ||
            frame->header.bits_per_sample != static_cast<std::uint32_t>(sourceFormat.precisionBits))
        {
          // Keep only inline evidence here; diagnostic formatting belongs outside
          // the C callback, and the delivered frame's storage is only borrowed.
          impl->optFormatMismatch = FlacFrameFormatMismatch{
            .expectedFormat = sourceFormat,
            .frameSampleRate = frame->header.sample_rate,
            .frameChannels = frame->header.channels,
            .framePrecisionBits = frame->header.bits_per_sample,
            .location = std::source_location::current(),
          };

          return ::FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        // A loss-of-sync notice can suppress libFLAC's MISSING_FRAME report.
        // Its repair path can then deliver silence and the real frame in one
        // process_single call. Do not overwrite pending PCM or relabel a later
        // frame as the next sample; even a lost first frame has no repair block.
        if (auto const expectedFrameIndex = impl->optSeekTarget.value_or(impl->nextFrameIndex);
            frameSampleNumberFor(frame) != expectedFrameIndex || impl->bufferedFrames != 0)
        {
          impl->optFrameDiscontinuity = FlacFrameDiscontinuity{
            .expectedFrameIndex = expectedFrameIndex,
            .frameIndex = frameSampleNumberFor(frame),
            .pendingFrames = impl->bufferedFrames,
            .location = std::source_location::current(),
          };

          return ::FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        auto const channels = frame->header.channels;
        auto const blockSize = frame->header.blocksize;
        std::uint8_t const sourceBitDepth = static_cast<std::uint8_t>(frame->header.bits_per_sample);
        auto const nativeEncoding = impl->outputAdapter.nativeFormat().encoding;
        auto const outputBitDepth = encodingNominalBits(nativeEncoding);

        if (nativeEncoding == SampleEncoding::Signed16Le)
        {
          impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * 2);
          auto* out = utility::layout::viewMutable<std::int16_t>(impl->pcmBuffer);

          for (std::uint32_t i = 0; i < blockSize; ++i)
          {
            for (std::uint32_t ch = 0; ch < channels; ++ch)
            {
              *out++ = static_cast<std::int16_t>(alignSample(buffer[ch][i], sourceBitDepth, outputBitDepth));
            }
          }
        }
        else if (nativeEncoding == SampleEncoding::Signed24PackedLe)
        {
          impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * 3);
          auto* out = utility::layout::viewMutable<std::uint8_t>(impl->pcmBuffer);

          for (std::uint32_t i = 0; i < blockSize; ++i)
          {
            for (std::uint32_t ch = 0; ch < channels; ++ch)
            {
              auto const val = alignSample(buffer[ch][i], sourceBitDepth, outputBitDepth);
              *out++ = static_cast<std::uint8_t>(val & kLowByteMask);
              *out++ = static_cast<std::uint8_t>((val >> 8) & kLowByteMask);
              *out++ = static_cast<std::uint8_t>((val >> 16) & kLowByteMask);
            }
          }
        }
        else if (nativeEncoding == SampleEncoding::Signed32Le)
        {
          impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * 4);
          auto const dst = utility::layout::viewArrayMutable<std::int32_t>(impl->pcmBuffer);

          impl->channelSpans.resize(channels);

          for (std::uint32_t ch = 0; ch < channels; ++ch)
          {
            impl->channelSpans[ch] = {buffer[ch], blockSize};
          }

          interleaveAndPadPcmSamples<std::int32_t, std::int32_t>(
            impl->channelSpans, dst, static_cast<std::uint8_t>(32U - sourceBitDepth));
        }
        else
        {
          return ::FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        impl->bufferedFrames = blockSize;
        auto convertedRes = impl->outputAdapter.convert(impl->pcmBuffer);

        if (!convertedRes)
        {
          impl->optOutputError = convertedRes.error();
          return ::FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        impl->outputBytes = *convertedRes;

        return ::FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
      });
  }

  void FlacDecoderSession::Impl::metadataCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                  ::FLAC__StreamMetadata const* metadata,
                                                  void* clientData) noexcept
  {
    invokeFlacCallback(
      "FLAC metadata callback",
      [&]
      {
        if (auto* const impl = utility::unsafeDowncast<Impl>(clientData);
            metadata->type == ::FLAC__METADATA_TYPE_STREAMINFO)
        {
          auto const& streamInfo = streamInfoFor(metadata);

          impl->info.sourceFormat.channels = static_cast<std::uint8_t>(streamInfo.channels);
          impl->info.sourceFormat.sampleRate = streamInfo.sample_rate;
          impl->info.sourceFormat.precisionBits = static_cast<std::uint8_t>(streamInfo.bits_per_sample);
          impl->info.sourceFormat.sampleKind = SampleKind::Integer;

          auto const configuredRes = impl->outputAdapter.configure(
            impl->info.sourceFormat, nativeFlacEncoding(impl->info.sourceFormat.precisionBits));

          if (!configuredRes)
          {
            impl->optOutputError = configuredRes.error();
            return;
          }

          impl->info.outputFormat = *configuredRes;
          impl->totalFrames = streamInfo.total_samples;

          if (streamInfo.sample_rate > 0)
          {
            impl->info.duration = detail::convertToDuration(streamInfo.total_samples, streamInfo.sample_rate);
          }

          impl->info.isLossy = false;
          impl->info.codec = AudioCodec::Flac;
          impl->hasConfiguredStreamInfo = true;
        }
      });
  }

  void FlacDecoderSession::Impl::errorCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                               ::FLAC__StreamDecoderErrorStatus status,
                                               void* clientData) noexcept
  {
    if (auto* const impl = utility::unsafeDowncast<Impl>(clientData);
        status != ::FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC && !impl->optDecodeError)
    {
      impl->optDecodeError = status;
    }
  }
} // namespace ao::audio
