// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/FlacDecoderSession.h>

#include <FLAC/stream_decoder.h>
#include <algorithm>
#include <ao/audio/PcmConverter.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/MappedFile.h>
#include <cstring>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kLowByteMask = 0xFF;
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    ::FLAC__StreamMetadata_StreamInfo const& getStreamInfo(::FLAC__StreamMetadata const* metadata)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      return metadata->data.stream_info;
    }
  }

  struct FlacDecoderSession::Impl final
  {
    Format requestedOutput;
    ::FLAC__StreamDecoder* decoder = nullptr;

    utility::MappedFile mappedFile;
    std::uint64_t currentOffset = 0;

    DecodedStreamInfo info;

    // Buffer for decoded samples
    std::vector<std::byte> pcmBuffer;
    std::uint32_t bufferedFrames = 0;
    std::uint64_t nextFrameIndex = 0;
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
  };

  FlacDecoderSession::FlacDecoderSession(Format outputFormat)
    : _impl{std::make_unique<Impl>(outputFormat)}
  {
  }

  FlacDecoderSession::~FlacDecoderSession() = default;

  Result<> FlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    if (auto const mapResult = _impl->mappedFile.map(filePath); !mapResult)
    {
      return std::unexpected(mapResult.error());
    }

    _impl->currentOffset = 0;
    _impl->eof = false;
    _impl->nextFrameIndex = 0;

    auto const initStatus = ::FLAC__stream_decoder_init_stream(_impl->decoder,
                                                               Impl::readCallback,
                                                               Impl::seekCallback,
                                                               Impl::tellCallback,
                                                               Impl::lengthCallback,
                                                               Impl::eofCallback,
                                                               Impl::writeCallback,
                                                               Impl::metadataCallback,
                                                               Impl::errorCallback,
                                                               _impl.get());

    if (initStatus != ::FLAC__STREAM_DECODER_INIT_STATUS_OK)
    {
      return makeError(Error::Code::InitFailed, "Failed to initialize FLAC decoder");
    }

    // Process until metadata is read
    if (::FLAC__stream_decoder_process_until_end_of_metadata(_impl->decoder) == 0)
    {
      return makeError(Error::Code::DecodeFailed, "Failed to read FLAC metadata");
    }

    return {};
  }

  void FlacDecoderSession::close()
  {
    if (_impl->decoder != nullptr)
    {
      ::FLAC__stream_decoder_finish(_impl->decoder);
    }

    _impl->mappedFile.unmap();
    _impl->pcmBuffer.clear();
    _impl->bufferedFrames = 0;
  }

  Result<> FlacDecoderSession::seek(std::uint32_t positionMs)
  {
    _impl->pcmBuffer.clear();
    _impl->bufferedFrames = 0;
    _impl->eof = false;

    auto const sampleRate = _impl->info.sourceFormat.sampleRate;

    if (sampleRate == 0)
    {
      return makeError(Error::Code::SeekFailed, "Sample rate is 0");
    }

    auto const targetSample = static_cast<::FLAC__uint64>(positionMs) * sampleRate / 1000;

    if (::FLAC__stream_decoder_seek_absolute(_impl->decoder, targetSample) == 0)
    {
      return makeError(Error::Code::SeekFailed, "FLAC seek failed");
    }

    _impl->nextFrameIndex = targetSample;

    return {};
  }

  void FlacDecoderSession::flush()
  {
    ::FLAC__stream_decoder_flush(_impl->decoder);
    _impl->pcmBuffer.clear();
    _impl->bufferedFrames = 0;
  }

  Result<PcmBlock> FlacDecoderSession::readNextBlock()
  {
    if (_impl->eof && _impl->bufferedFrames == 0)
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    // If buffer is empty, process a single frame
    while (_impl->bufferedFrames == 0 && !_impl->eof)
    {
      if (::FLAC__stream_decoder_process_single(_impl->decoder) == 0)
      {
        if (::FLAC__stream_decoder_get_state(_impl->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM)
        {
          _impl->eof = true;
          break;
        }

        return makeError(Error::Code::DecodeFailed, "FLAC process single failed");
      }

      if (::FLAC__stream_decoder_get_state(_impl->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM)
      {
        _impl->eof = true;
        break;
      }
    }

    if (_impl->bufferedFrames == 0 && _impl->eof)
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto block = PcmBlock{
      .bytes = _impl->pcmBuffer,
      .bitDepth = _impl->info.outputFormat.bitDepth,
      .frames = _impl->bufferedFrames,
      .firstFrameIndex = _impl->nextFrameIndex,
      .endOfStream =
        _impl->eof && (::FLAC__stream_decoder_get_state(_impl->decoder) == ::FLAC__STREAM_DECODER_END_OF_STREAM),
    };

    _impl->nextFrameIndex += _impl->bufferedFrames;
    _impl->bufferedFrames = 0;

    return block;
  }

  DecodedStreamInfo FlacDecoderSession::streamInfo() const
  {
    return _impl->info;
  }

  // Implementation of callbacks

  ::FLAC__StreamDecoderReadStatus FlacDecoderSession::Impl::readCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                         ::FLAC__byte* buffer,
                                                                         std::size_t* bytes,
                                                                         void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    auto const fileBytes = impl->mappedFile.bytes();

    if (*bytes > 0)
    {
      auto const remaining = fileBytes.size() - impl->currentOffset;

      if (remaining == 0)
      {
        *bytes = 0;
        return ::FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
      }

      auto const toRead = std::min<std::size_t>(*bytes, remaining);
      std::memcpy(buffer, fileBytes.data() + impl->currentOffset, toRead);
      impl->currentOffset += toRead;
      *bytes = toRead;

      return ::FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    return ::FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  ::FLAC__StreamDecoderSeekStatus FlacDecoderSession::Impl::seekCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                         ::FLAC__uint64 absoluteByteOffset,
                                                                         void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    auto const fileBytes = impl->mappedFile.bytes();

    if (absoluteByteOffset >= fileBytes.size())
    {
      return ::FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }

    impl->currentOffset = absoluteByteOffset;

    return ::FLAC__STREAM_DECODER_SEEK_STATUS_OK;
  }

  ::FLAC__StreamDecoderTellStatus FlacDecoderSession::Impl::tellCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                         ::FLAC__uint64* absoluteByteOffset,
                                                                         void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    *absoluteByteOffset = impl->currentOffset;

    return ::FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  ::FLAC__StreamDecoderLengthStatus FlacDecoderSession::Impl::lengthCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                                                             ::FLAC__uint64* streamLength,
                                                                             void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    *streamLength = impl->mappedFile.bytes().size();

    return ::FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }

  ::FLAC__bool FlacDecoderSession::Impl::eofCallback(::FLAC__StreamDecoder const* /*decoder*/, void* clientData)
  {
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);

    return static_cast<::FLAC__bool>(impl->currentOffset >= impl->mappedFile.bytes().size());
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
          *out++ = static_cast<std::int16_t>(buffer[ch][i]);
        }
      }
    }
    else if (outBps == 24)
    {
      impl->pcmBuffer.resize(static_cast<std::size_t>(blockSize) * channels * kBytesPer24BitSample);
      auto* out = utility::layout::asMutablePtr<std::uint8_t>(impl->pcmBuffer);

      for (std::uint32_t i = 0; i < blockSize; ++i)
      {
        for (std::uint32_t ch = 0; channels > 0 && ch < channels; ++ch)
        {
          auto const val = static_cast<std::int32_t>(buffer[ch][i]);
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

      auto channelSpans = std::vector<std::span<std::int32_t const>>(channels);

      for (std::uint32_t ch = 0; ch < channels; ++ch)
      {
        channelSpans[ch] = {buffer[ch], blockSize};
      }

      PcmConverter::interleaveAndPad<std::int32_t, std::int32_t>(
        channelSpans, dst, static_cast<std::uint8_t>(32 - bps));
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
    auto* const impl = utility::unsafeDowncast<Impl>(clientData);

    if (metadata->type == ::FLAC__METADATA_TYPE_STREAMINFO)
    {
      auto const& streamInfo = getStreamInfo(metadata);

      impl->info.sourceFormat.channels = static_cast<std::uint8_t>(streamInfo.channels);
      impl->info.sourceFormat.sampleRate = streamInfo.sample_rate;
      impl->info.sourceFormat.bitDepth = static_cast<std::uint8_t>(streamInfo.bits_per_sample);
      impl->info.sourceFormat.validBits = impl->info.sourceFormat.bitDepth;
      impl->info.sourceFormat.isFloat = false;
      impl->info.sourceFormat.isInterleaved = true;

      impl->info.outputFormat = impl->info.sourceFormat;

      if (impl->requestedOutput.bitDepth != 0)
      {
        impl->info.outputFormat.bitDepth = impl->requestedOutput.bitDepth;
        impl->info.outputFormat.validBits =
          (impl->requestedOutput.validBits != 0) ? impl->requestedOutput.validBits : impl->requestedOutput.bitDepth;
      }

      if (streamInfo.sample_rate > 0)
      {
        impl->info.durationMs = static_cast<std::uint32_t>(streamInfo.total_samples * 1000 / streamInfo.sample_rate);
      }

      impl->info.isLossy = false;
    }
  }

  void FlacDecoderSession::Impl::errorCallback(::FLAC__StreamDecoder const* /*decoder*/,
                                               ::FLAC__StreamDecoderErrorStatus /*status*/,
                                               void* clientData)
  {
    [[maybe_unused]] auto* const impl = utility::unsafeDowncast<Impl>(clientData);
    /* TODO logging */
  }
} // namespace ao::audio