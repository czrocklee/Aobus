// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/decoder/FlacDecoderSession.h"
#include "core/Log.h"

#include <FLAC/stream_decoder.h>
#include <algorithm>
#include <cstring>
#include <rs/utility/MappedFile.h>

namespace app::core::decoder
{

  struct FlacDecoderSession::Impl
  {
    playback::StreamFormat requestedOutput;
    FLAC__StreamDecoder* decoder = nullptr;

    rs::utility::MappedFile mappedFile;
    std::uint64_t currentOffset = 0;

    DecodedStreamInfo info;
    std::string error;

    // Buffer for decoded samples
    std::vector<std::byte> pcmBuffer;
    std::uint32_t bufferedFrames = 0;
    std::uint64_t nextFrameIndex = 0;
    bool eof = false;

    Impl(playback::StreamFormat output)
      : requestedOutput(output)
    {
      decoder = ::FLAC__stream_decoder_new();
    }

    ~Impl()
    {
      if (decoder)
      {
        ::FLAC__stream_decoder_delete(decoder);
      }
    }

    // Callbacks for libFLAC
    static FLAC__StreamDecoderReadStatus readCallback(FLAC__StreamDecoder const* decoder,
                                                      FLAC__byte buffer[],
                                                      size_t* bytes,
                                                      void* clientData);
    static FLAC__StreamDecoderSeekStatus seekCallback(FLAC__StreamDecoder const* decoder,
                                                      FLAC__uint64 absoluteByteOffset,
                                                      void* clientData);
    static FLAC__StreamDecoderTellStatus tellCallback(FLAC__StreamDecoder const* decoder,
                                                      FLAC__uint64* absoluteByteOffset,
                                                      void* clientData);
    static FLAC__StreamDecoderLengthStatus lengthCallback(FLAC__StreamDecoder const* decoder,
                                                          FLAC__uint64* streamLength,
                                                          void* clientData);
    static FLAC__bool eofCallback(FLAC__StreamDecoder const* decoder, void* clientData);

    static FLAC__StreamDecoderWriteStatus writeCallback(FLAC__StreamDecoder const* decoder,
                                                        FLAC__Frame const* frame,
                                                        FLAC__int32 const* const buffer[],
                                                        void* clientData);
    static void metadataCallback(FLAC__StreamDecoder const* decoder,
                                 FLAC__StreamMetadata const* metadata,
                                 void* clientData);
    static void errorCallback(FLAC__StreamDecoder const* decoder,
                              FLAC__StreamDecoderErrorStatus status,
                              void* clientData);
  };

  FlacDecoderSession::FlacDecoderSession(playback::StreamFormat outputFormat)
    : _impl(std::make_unique<Impl>(outputFormat))
  {
  }

  FlacDecoderSession::~FlacDecoderSession() = default;

  bool FlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    auto const mapError = _impl->mappedFile.map(filePath);

    if (!mapError.empty())
    {
      _impl->error = mapError;
      return false;
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

    if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK)
    {
      _impl->error = "Failed to initialize FLAC decoder";
      return false;
    }

    // Process until metadata is read
    if (!::FLAC__stream_decoder_process_until_end_of_metadata(_impl->decoder))
    {
      _impl->error = "Failed to read FLAC metadata";
      return false;
    }

    return true;
  }

  void FlacDecoderSession::close()
  {
    if (_impl->decoder)
    {
      ::FLAC__stream_decoder_finish(_impl->decoder);
    }

    _impl->mappedFile.unmap();
    _impl->pcmBuffer.clear();
    _impl->bufferedFrames = 0;
  }

  bool FlacDecoderSession::seek(std::uint32_t positionMs)
  {
    _impl->pcmBuffer.clear();
    _impl->bufferedFrames = 0;
    _impl->eof = false;

    auto const sampleRate = _impl->info.sourceFormat.sampleRate;

    if (sampleRate == 0)
    {
      return false;
    }

    auto const targetSample = static_cast<FLAC__uint64>(positionMs) * sampleRate / 1000;

    if (!::FLAC__stream_decoder_seek_absolute(_impl->decoder, targetSample))
    {
      _impl->error = "FLAC seek failed";
      return false;
    }

    _impl->nextFrameIndex = targetSample;
    return true;
  }

  void FlacDecoderSession::flush()
  {
    ::FLAC__stream_decoder_flush(_impl->decoder);
    _impl->pcmBuffer.clear();
    _impl->bufferedFrames = 0;
  }

  std::optional<PcmBlock> FlacDecoderSession::readNextBlock()
  {
    if (_impl->eof && _impl->bufferedFrames == 0)
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    // If buffer is empty, process a single frame
    while (_impl->bufferedFrames == 0 && !_impl->eof)
    {
      if (!::FLAC__stream_decoder_process_single(_impl->decoder))
      {
        if (::FLAC__stream_decoder_get_state(_impl->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM)
        {
          _impl->eof = true;
          break;
        }

        return std::nullopt;
      }
    }

    if (_impl->bufferedFrames == 0 && _impl->eof)
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto block = PcmBlock{};
    block.bytes = std::move(_impl->pcmBuffer);
    block.bitDepth = _impl->info.outputFormat.bitDepth;
    block.frames = _impl->bufferedFrames;
    block.firstFrameIndex = _impl->nextFrameIndex;
    block.endOfStream =
      _impl->eof && (::FLAC__stream_decoder_get_state(_impl->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM);

    _impl->nextFrameIndex += _impl->bufferedFrames;
    _impl->bufferedFrames = 0;

    return block;
  }

  DecodedStreamInfo FlacDecoderSession::streamInfo() const { return _impl->info; }

  std::string_view FlacDecoderSession::lastError() const noexcept { return _impl->error; }

  // Implementation of callbacks

  FLAC__StreamDecoderReadStatus FlacDecoderSession::Impl::readCallback(FLAC__StreamDecoder const* /*decoder*/,
                                                                       FLAC__byte buffer[],
                                                                       size_t* bytes,
                                                                       void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);
    auto const fileBytes = impl->mappedFile.bytes();

    if (*bytes > 0)
    {
      auto const remaining = fileBytes.size() - impl->currentOffset;

      if (remaining == 0)
      {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
      }

      auto const toRead = std::min<std::size_t>(*bytes, remaining);
      std::memcpy(buffer, fileBytes.data() + impl->currentOffset, toRead);
      impl->currentOffset += toRead;
      *bytes = toRead;
      return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  FLAC__StreamDecoderSeekStatus FlacDecoderSession::Impl::seekCallback(FLAC__StreamDecoder const* /*decoder*/,
                                                                       FLAC__uint64 absoluteByteOffset,
                                                                       void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);
    auto const fileBytes = impl->mappedFile.bytes();

    if (absoluteByteOffset >= fileBytes.size())
    {
      return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }

    impl->currentOffset = absoluteByteOffset;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
  }

  FLAC__StreamDecoderTellStatus FlacDecoderSession::Impl::tellCallback(FLAC__StreamDecoder const* /*decoder*/,
                                                                       FLAC__uint64* absoluteByteOffset,
                                                                       void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);
    *absoluteByteOffset = impl->currentOffset;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  FLAC__StreamDecoderLengthStatus FlacDecoderSession::Impl::lengthCallback(FLAC__StreamDecoder const* /*decoder*/,
                                                                           FLAC__uint64* streamLength,
                                                                           void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);
    *streamLength = impl->mappedFile.bytes().size();
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }

  FLAC__bool FlacDecoderSession::Impl::eofCallback(FLAC__StreamDecoder const* /*decoder*/, void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);
    return impl->currentOffset >= impl->mappedFile.bytes().size();
  }

  FLAC__StreamDecoderWriteStatus FlacDecoderSession::Impl::writeCallback(FLAC__StreamDecoder const* /*decoder*/,
                                                                         FLAC__Frame const* frame,
                                                                         FLAC__int32 const* const buffer[],
                                                                         void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);

    auto const channels = frame->header.channels;
    auto const bps = frame->header.bits_per_sample;
    auto const blockSize = frame->header.blocksize;

    auto const outBps = (impl->requestedOutput.bitDepth != 0) ? impl->requestedOutput.bitDepth : bps;

    if (outBps == 16)
    {
      impl->pcmBuffer.resize(blockSize * channels * 2);
      auto* out = reinterpret_cast<std::int16_t*>(impl->pcmBuffer.data());

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
      impl->pcmBuffer.resize(blockSize * channels * 3);
      auto* out = reinterpret_cast<std::uint8_t*>(impl->pcmBuffer.data());

      for (std::uint32_t i = 0; i < blockSize; ++i)
      {
        for (std::uint32_t ch = 0; channels > 0 && ch < channels; ++ch)
        {
          auto const val = static_cast<std::int32_t>(buffer[ch][i]);
          *out++ = static_cast<std::uint8_t>(val & 0xFF);
          *out++ = static_cast<std::uint8_t>((val >> 8) & 0xFF);
          *out++ = static_cast<std::uint8_t>((val >> 16) & 0xFF);
        }
      }
    }
    else
    {
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    impl->bufferedFrames = blockSize;
    impl->info.outputFormat.bitDepth = static_cast<std::uint8_t>(outBps);
    impl->info.outputFormat.channels = static_cast<std::uint8_t>(channels);
    impl->info.outputFormat.sampleRate = frame->header.sample_rate;

    return ::FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  void FlacDecoderSession::Impl::metadataCallback(FLAC__StreamDecoder const* /*decoder*/,
                                                  FLAC__StreamMetadata const* metadata,
                                                  void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
      impl->info.sourceFormat.channels = static_cast<std::uint8_t>(metadata->data.stream_info.channels);
      impl->info.sourceFormat.sampleRate = metadata->data.stream_info.sample_rate;
      impl->info.sourceFormat.bitDepth = static_cast<std::uint8_t>(metadata->data.stream_info.bits_per_sample);
      impl->info.sourceFormat.isFloat = false;
      impl->info.sourceFormat.isInterleaved = true;

      impl->info.outputFormat = impl->info.sourceFormat;

      if (impl->requestedOutput.bitDepth != 0)
      {
        impl->info.outputFormat.bitDepth = impl->requestedOutput.bitDepth;
      }

      if (metadata->data.stream_info.sample_rate > 0)
      {
        impl->info.durationMs = static_cast<std::uint32_t>(metadata->data.stream_info.total_samples * 1000 /
                                                           metadata->data.stream_info.sample_rate);
      }

      impl->info.isLossy = false;
    }
  }

  void FlacDecoderSession::Impl::errorCallback(FLAC__StreamDecoder const* /*decoder*/,
                                               FLAC__StreamDecoderErrorStatus status,
                                               void* clientData)
  {
    auto* impl = static_cast<Impl*>(clientData);
    impl->error = std::format("FLAC error: {}", ::FLAC__StreamDecoderErrorStatusString[status]);
  }

} // namespace app::core::decoder