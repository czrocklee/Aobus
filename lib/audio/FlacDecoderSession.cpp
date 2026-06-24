// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/MappedFileCursor.h"
#include "detail/OutputFormatValidation.h"
#include "detail/TimeConversion.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
#include <ao/audio/Types.h>
#include <ao/audio/detail/DecoderError.h>
#include <ao/utility/ByteView.h>

#include <FLAC/format.h>
#include <FLAC/ordinals.h>
#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kLowByteMask = 0xFF;

    ::FLAC__StreamMetadata_StreamInfo const& getStreamInfo(::FLAC__StreamMetadata const* metadata)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      return metadata->data.stream_info;
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
    Format requestedOutput;
    ::FLAC__StreamDecoder* decoder = nullptr;

    detail::MappedFileCursor fileCursor;

    DecodedStreamInfo info;

    // Buffer for decoded samples
    std::vector<std::byte> pcmBuffer;
    // Reused per-channel span scratch for interleaving (avoids a per-block heap
    // allocation in the write callback). Capacity persists across blocks.
    std::vector<std::span<std::int32_t const>> channelSpans;
    std::uint32_t bufferedFrames = 0;
    std::uint64_t nextFrameIndex = 0;
    std::uint64_t totalFrames = 0;
    std::optional<::FLAC__StreamDecoderErrorStatus> optDecodeError;
    bool eof = false;

    Impl(Format const& output)
      : requestedOutput{output}, decoder{::FLAC__stream_decoder_new()}
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

    // Callbacks for libFLAC
    static ::FLAC__StreamDecoderReadStatus readCallback(::FLAC__StreamDecoder const* decoder,
                                                        ::FLAC__byte* buffer,
                                                        std::size_t* bytes,
                                                        void* clientData);

    static ::FLAC__StreamDecoderSeekStatus seekCallback(::FLAC__StreamDecoder const* decoder,
                                                        ::FLAC__uint64 absoluteByteOffset,
                                                        void* clientData);

    static ::FLAC__StreamDecoderTellStatus tellCallback(::FLAC__StreamDecoder const* decoder,
                                                        ::FLAC__uint64* absoluteByteOffset,
                                                        void* clientData);

    static ::FLAC__StreamDecoderLengthStatus lengthCallback(::FLAC__StreamDecoder const* decoder,
                                                            ::FLAC__uint64* streamLength,
                                                            void* clientData);

    static ::FLAC__bool eofCallback(::FLAC__StreamDecoder const* decoder, void* clientData);

    static ::FLAC__StreamDecoderWriteStatus writeCallback(::FLAC__StreamDecoder const* decoder,
                                                          ::FLAC__Frame const* frame,
                                                          ::FLAC__int32 const* const* buffer,
                                                          void* clientData);

    static void metadataCallback(::FLAC__StreamDecoder const* decoder,
                                 ::FLAC__StreamMetadata const* metadata,
                                 void* clientData);

    static void errorCallback(::FLAC__StreamDecoder const* decoder,
                              ::FLAC__StreamDecoderErrorStatus status,
                              void* clientData);

    void checkDecodeError() const
    {
      if (optDecodeError)
      {
        detail::throwDecoderError(
          Error::Code::DecodeFailed,
          std::string{"FLAC decode error: "} + ::FLAC__StreamDecoderErrorStatusString[*optDecodeError]);
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

  FlacDecoderSession::FlacDecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  FlacDecoderSession::~FlacDecoderSession() = default;

  Result<> FlacDecoderSession::openCodec(std::filesystem::path const& filePath)
  {
    try
    {
      if (auto const result = _implPtr->fileCursor.open(filePath); !result)
      {
        detail::throwDecoderError(result.error());
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

      if (auto const result =
            detail::validateFixedOutputRequest(_implPtr->requestedOutput, _implPtr->info.outputFormat, "FLAC");
          !result)
      {
        detail::throwDecoderError(result.error());
      }

      auto const outputBitDepth = _implPtr->info.outputFormat.bitDepth;
      auto const expectedValidBits = std::min(_implPtr->info.sourceFormat.validBits, outputBitDepth);

      if (_implPtr->requestedOutput.isFloat || (outputBitDepth != 16 && outputBitDepth != 24 && outputBitDepth != 32) ||
          _implPtr->info.outputFormat.validBits != expectedValidBits)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported FLAC output sample format");
      }

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  void FlacDecoderSession::close() noexcept
  {
    if (_implPtr->decoder != nullptr)
    {
      ::FLAC__stream_decoder_finish(_implPtr->decoder);
    }

    _implPtr->fileCursor.close();
    _implPtr->pcmBuffer.clear();
    _implPtr->bufferedFrames = 0;
    _implPtr->nextFrameIndex = 0;
    _implPtr->totalFrames = 0;
    _implPtr->optDecodeError.reset();
    _implPtr->eof = false;
    _implPtr->info = {};
  }

  Result<> FlacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    try
    {
      _implPtr->pcmBuffer.clear();
      _implPtr->bufferedFrames = 0;
      _implPtr->optDecodeError.reset();
      _implPtr->eof = false;

      auto const sampleRate = _implPtr->info.sourceFormat.sampleRate;

      if (sampleRate == 0)
      {
        detail::throwDecoderError(Error::Code::SeekFailed, "Sample rate is 0");
      }

      auto const targetSample = static_cast<::FLAC__uint64>(durationToSamples(offset, sampleRate));

      if (::FLAC__stream_decoder_seek_absolute(_implPtr->decoder, targetSample) == 0)
      {
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
      ::FLAC__stream_decoder_flush(_implPtr->decoder);
    }

    _implPtr->pcmBuffer.clear();
    _implPtr->bufferedFrames = 0;
  }

  Result<PcmBlock> FlacDecoderSession::readNextBlock() noexcept
  {
    try
    {
      if (!_implPtr->fileCursor.isOpen())
      {
        return PcmBlock{.bytes = {}, .endOfStream = true};
      }

      _implPtr->checkDecodeError();

      if (_implPtr->eof && _implPtr->bufferedFrames == 0)
      {
        return PcmBlock{.bytes = {}, .endOfStream = true};
      }

      // If buffer is empty, process a single frame
      while (_implPtr->bufferedFrames == 0 && !_implPtr->eof)
      {
        if (::FLAC__stream_decoder_process_single(_implPtr->decoder) == 0)
        {
          if (::FLAC__stream_decoder_get_state(_implPtr->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM)
          {
            _implPtr->checkEndOfStream();

            _implPtr->eof = true;
            break;
          }

          detail::throwDecoderError(Error::Code::DecodeFailed, "FLAC process single failed");
        }

        _implPtr->checkDecodeError();

        if (::FLAC__stream_decoder_get_state(_implPtr->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM)
        {
          _implPtr->checkEndOfStream();

          _implPtr->eof = true;
          break;
        }
      }

      if (_implPtr->bufferedFrames == 0 && _implPtr->eof)
      {
        return PcmBlock{.bytes = {}, .endOfStream = true};
      }

      auto block = PcmBlock{
        .bytes = _implPtr->pcmBuffer,
        .bitDepth = _implPtr->info.outputFormat.bitDepth,
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
                                                                         void* clientData)
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
                                                                         void* clientData)
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
                                                                         void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    *absoluteByteOffset = impl->fileCursor.position();

    return ::FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  ::FLAC__StreamDecoderLengthStatus FlacDecoderSession::Impl::lengthCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                             ::FLAC__uint64* streamLength,
                                                                             void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    *streamLength = impl->fileCursor.size();

    return ::FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }

  ::FLAC__bool FlacDecoderSession::Impl::eofCallback(::FLAC__StreamDecoder const* /*decoder*/, void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);

    return static_cast<::FLAC__bool>(impl->fileCursor.atEnd());
  }

  ::FLAC__StreamDecoderWriteStatus FlacDecoderSession::Impl::writeCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                           ::FLAC__Frame const* frame,
                                                                           ::FLAC__int32 const* const* buffer,
                                                                           void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);

    auto const channels = frame->header.channels;
    auto const bps = frame->header.bits_per_sample;
    auto const blockSize = frame->header.blocksize;

    auto const outBps = (impl->requestedOutput.bitDepth != 0) ? impl->requestedOutput.bitDepth : bps;

    if (outBps == 16)
    {
      impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * 2);
      auto* out = utility::layout::asMutablePtr<std::int16_t>(impl->pcmBuffer);

      for (std::uint32_t i = 0; i < blockSize; ++i)
      {
        for (std::uint32_t ch = 0; channels > 0 && ch < channels; ++ch)
        {
          *out++ = static_cast<std::int16_t>(alignSample(buffer[ch][i], bps, outBps));
        }
      }
    }
    else if (outBps == 24)
    {
      impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * 3);
      auto* out = utility::layout::asMutablePtr<std::uint8_t>(impl->pcmBuffer);

      for (std::uint32_t i = 0; i < blockSize; ++i)
      {
        for (std::uint32_t ch = 0; channels > 0 && ch < channels; ++ch)
        {
          auto const val = alignSample(buffer[ch][i], bps, outBps);
          *out++ = static_cast<std::uint8_t>(val & kLowByteMask);
          *out++ = static_cast<std::uint8_t>((val >> 8) & kLowByteMask);
          *out++ = static_cast<std::uint8_t>((val >> 16) & kLowByteMask);
        }
      }
    }
    else if (outBps == 32)
    {
      impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * 4);
      auto const dst = utility::layout::viewArrayMutable<std::int32_t>(impl->pcmBuffer);

      impl->channelSpans.resize(channels);

      for (std::uint32_t ch = 0; ch < channels; ++ch)
      {
        impl->channelSpans[ch] = {buffer[ch], blockSize};
      }

      PcmConverter::interleaveAndPad<std::int32_t, std::int32_t>(
        impl->channelSpans, dst, static_cast<std::uint8_t>(32 - bps));
    }
    else
    {
      return ::FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    impl->bufferedFrames = blockSize;
    impl->info.outputFormat.bitDepth = static_cast<std::uint8_t>(outBps);
    impl->info.outputFormat.channels = static_cast<std::uint8_t>(channels);
    impl->info.outputFormat.sampleRate = frame->header.sample_rate;

    return ::FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  void FlacDecoderSession::Impl::metadataCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                  ::FLAC__StreamMetadata const* metadata,
                                                  void* clientData)
  {
    if (auto* const impl = utility::unsafeDowncast<Impl>(clientData);
        metadata->type == ::FLAC__METADATA_TYPE_STREAMINFO)
    {
      auto const& streamInfo = getStreamInfo(metadata);

      impl->info.sourceFormat.channels = static_cast<std::uint8_t>(streamInfo.channels);
      impl->info.sourceFormat.sampleRate = streamInfo.sample_rate;
      impl->info.sourceFormat.bitDepth = static_cast<std::uint8_t>(streamInfo.bits_per_sample);
      impl->info.sourceFormat.validBits = impl->info.sourceFormat.bitDepth;
      impl->info.sourceFormat.isFloat = false;
      impl->info.sourceFormat.isInterleaved = true;

      impl->info.outputFormat = impl->info.sourceFormat;
      impl->totalFrames = streamInfo.total_samples;

      if (impl->requestedOutput.bitDepth != 0)
      {
        impl->info.outputFormat.bitDepth = impl->requestedOutput.bitDepth;
        impl->info.outputFormat.validBits =
          (impl->requestedOutput.validBits != 0)
            ? impl->requestedOutput.validBits
            : std::min(impl->info.sourceFormat.validBits, impl->requestedOutput.bitDepth);
      }

      if (streamInfo.sample_rate > 0)
      {
        impl->info.duration = detail::convertToDuration(streamInfo.total_samples, streamInfo.sample_rate);
      }

      impl->info.isLossy = false;
      impl->info.codec = AudioCodec::Flac;
    }
  }

  void FlacDecoderSession::Impl::errorCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                               ::FLAC__StreamDecoderErrorStatus status,
                                               void* clientData)
  {
    if (auto* const impl = utility::unsafeDowncast<Impl>(clientData);
        status != ::FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC && !impl->optDecodeError)
    {
      impl->optDecodeError = status;
    }
  }
} // namespace ao::audio
