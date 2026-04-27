// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/decoder/AlacDecoderSession.h"
#include "core/Log.h"

#include <alac/ALACBitUtilities.h>
#include <alac/ALACDecoder.h>

#include <rs/media/mp4/Demuxer.h>
#include <rs/utility/MappedFile.h>

namespace app::core::decoder
{
  using namespace rs::media::mp4;

  struct AlacDecoderSession::Impl
  {
    std::string error;
    DecodedStreamInfo info;

    std::unique_ptr<ALACDecoder> decoder;

    std::uint32_t currentSampleIndex = 0;
    std::uint32_t timescale = 0;

    rs::utility::MappedFile mappedFile;
    std::unique_ptr<Demuxer> demuxer;

    Impl(AudioFormat /*output*/) { decoder = std::make_unique<ALACDecoder>(); }

    void setError(std::string_view msg) { error = std::string(msg); }
  };

  AlacDecoderSession::AlacDecoderSession(AudioFormat outputFormat)
    : _impl(std::make_unique<Impl>(outputFormat))
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  bool AlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    auto const mapError = _impl->mappedFile.map(filePath);

    if (!mapError.empty())
    {
      _impl->setError(mapError);
      return false;
    }

    _impl->demuxer = std::make_unique<Demuxer>(_impl->mappedFile.bytes());
    auto const demuxError = _impl->demuxer->parseTrack("alac");

    if (!demuxError.empty())
    {
      _impl->setError(demuxError);
      return false;
    }

    auto const cookie = _impl->demuxer->magicCookie();
    _impl->decoder->Init(const_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t const*>(cookie.data())),
                         static_cast<uint32_t>(cookie.size()));

    _impl->timescale = _impl->demuxer->timescale();
    auto const duration = _impl->demuxer->duration();

    if (_impl->timescale > 0)
    {
      _impl->info.durationMs =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(duration) * 1000) / _impl->timescale);
    }

    _impl->info.sourceFormat.channels = 2;
    _impl->info.sourceFormat.sampleRate = _impl->timescale;
    _impl->info.sourceFormat.bitDepth = 16;
    _impl->info.outputFormat = _impl->info.sourceFormat;

    return true;
  }

  void AlacDecoderSession::close()
  {
    _impl->demuxer.reset();
    _impl->mappedFile.unmap();
    _impl->currentSampleIndex = 0;
  }

  bool AlacDecoderSession::seek(std::uint32_t /*positionMs*/)
  {
    if (_impl->timescale == 0)
    {
      return false;
    }

    _impl->currentSampleIndex = 0;
    return true;
  }

  void AlacDecoderSession::flush()
  {
  }

  std::optional<PcmBlock> AlacDecoderSession::readNextBlock()
  {
    if (!_impl->demuxer || _impl->currentSampleIndex >= _impl->demuxer->sampleCount())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const packet = _impl->demuxer->getSamplePayload(_impl->currentSampleIndex);

    if (packet.empty())
    {
      return std::nullopt;
    }

    std::uint32_t numFrames = 0;
    std::vector<std::byte> decodedPcm(1024 * 1024);

    auto bitBuffer = BitBuffer{};
    BitBufferInit(&bitBuffer,
                  const_cast<uint8_t*>(reinterpret_cast<uint8_t const*>(packet.data())),
                  static_cast<uint32_t>(packet.size()));

    auto const status = _impl->decoder->Decode(
      &bitBuffer, reinterpret_cast<uint8_t*>(decodedPcm.data()), 4096, _impl->info.outputFormat.channels, &numFrames);

    if (status != 0)
    {
      return std::nullopt;
    }

    auto block = PcmBlock{};
    auto const bytesPerFrame = _impl->info.outputFormat.channels * (_impl->info.outputFormat.bitDepth / 8);

    decodedPcm.resize(numFrames * bytesPerFrame);
    block.bytes = std::move(decodedPcm);
    block.frames = numFrames;
    block.bitDepth = _impl->info.outputFormat.bitDepth;

    _impl->currentSampleIndex++;
    block.endOfStream = (_impl->currentSampleIndex >= _impl->demuxer->sampleCount());

    return block;
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const
  {
    return _impl->info;
  }

  std::string_view AlacDecoderSession::lastError() const noexcept
  {
    return _impl->error;
  }

} // namespace app::core::decoder