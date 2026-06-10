// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/Mp3DecoderSession.h>
#include <ao/audio/detail/Mpg123Runtime.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/MappedFile.h>

#include <mpg123.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kMp3PcmBitDepth = 16;
    constexpr std::uint8_t kFloat32BitDepth = 32;
    constexpr double kMsPerSecond = 1000.0;

    std::uint8_t channelCountFromMpg123(std::int32_t channels) noexcept
    {
      return (channels == MPG123_MONO) ? 1U : 2U;
    }

    std::uint8_t channelCountFromFrameMode(std::int32_t mode) noexcept
    {
      return (mode == MPG123_M_MONO) ? 1U : 2U;
    }

    std::uint8_t bitDepthFromEncoding(int encoding) noexcept
    {
      auto const bytesPerSample = ::mpg123_encsize(encoding);

      if (bytesPerSample <= 0)
      {
        return 0U;
      }

      return static_cast<std::uint8_t>(bytesPerSample * 8);
    }
  }

  struct Mp3DecoderSession::Impl final
  {
    Format requestedOutput;
    detail::Mpg123EnvironmentGuard mpg123Environment;
    mpg123_handle* mh = nullptr;
    utility::MappedFile mappedFile;
    std::uint64_t currentOffset = 0;
    DecodedStreamInfo info;
    std::vector<std::byte> decodeBuffer;
    std::uint64_t nextFrameIndex = 0;
    bool eof = false;

    Impl(Format const& output)
      : requestedOutput{output}
    {
      int err = MPG123_OK;
      mh = ::mpg123_new(nullptr, &err);
    }

    ~Impl()
    {
      if (mh != nullptr)
      {
        ::mpg123_delete(mh);
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    static ssize_t readCb(void* handle, void* buf, size_t sz)
    {
      auto* self = static_cast<Impl*>(handle);
      auto const bytes = self->mappedFile.bytes();

      if (self->currentOffset >= bytes.size())
      {
        return 0;
      }

      size_t const toRead = std::min(sz, static_cast<size_t>(bytes.size() - self->currentOffset));
      std::memcpy(buf, bytes.data() + self->currentOffset, toRead);
      self->currentOffset += toRead;
      return static_cast<ssize_t>(toRead);
    }

    static off_t lseekCb(void* handle, off_t offset, std::int32_t whence)
    {
      auto* self = static_cast<Impl*>(handle);
      auto const bytes = self->mappedFile.bytes();
      off_t newPos = 0;

      switch (whence)
      {
        case SEEK_SET: newPos = offset; break;
        case SEEK_CUR: newPos = static_cast<off_t>(self->currentOffset) + offset; break;
        case SEEK_END: newPos = static_cast<off_t>(bytes.size()) + offset; break;
        default: return -1;
      }

      if (newPos < 0 || static_cast<size_t>(newPos) > bytes.size())
      {
        return -1;
      }

      self->currentOffset = static_cast<size_t>(newPos);
      return newPos;
    }

    Result<> configureOutputFormat() const
    {
      if (mh == nullptr)
      {
        return makeError(Error::Code::InitFailed, "Failed to create MP3 handle");
      }

      if (::mpg123_format_none(mh) != MPG123_OK)
      {
        return makeError(Error::Code::InitFailed, "Failed to reset MP3 output formats");
      }

      auto const encoding = [&] -> std::int32_t
      {
        if (requestedOutput.isFloat)
        {
          if (requestedOutput.bitDepth == 0 || requestedOutput.bitDepth == kFloat32BitDepth)
          {
            return MPG123_ENC_FLOAT_32; // NOLINT(misc-include-cleaner)
          }

          return 0;
        }

        if (requestedOutput.bitDepth == 0 || requestedOutput.bitDepth == kMp3PcmBitDepth)
        {
          return MPG123_ENC_SIGNED_16; // NOLINT(misc-include-cleaner)
        }

        return 0;
      }();

      if (encoding == 0)
      {
        return makeError(Error::Code::NotSupported, "Unsupported MP3 output sample format");
      }

      if (requestedOutput.channels > 2)
      {
        return makeError(Error::Code::NotSupported, "Unsupported MP3 channel count");
      }

      if (::mpg123_format2(mh, 0, MPG123_MONO | MPG123_STEREO, encoding) != MPG123_OK)
      {
        return makeError(Error::Code::NotSupported, "Unsupported MP3 output format");
      }

      return {};
    }

    Result<> refreshStreamInfo()
    {
      long rate = 0;
      int channels = 0;
      int encoding = 0;

      if (::mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK)
      {
        return makeError(Error::Code::DecodeFailed, "Failed to get MP3 format");
      }

      auto actualOutput = Format{
        .sampleRate = static_cast<std::uint32_t>(rate),
        .channels = channelCountFromMpg123(channels),
        .bitDepth = bitDepthFromEncoding(encoding),
        .isFloat = (encoding & MPG123_ENC_FLOAT) != 0, // NOLINT(misc-include-cleaner)
        .isInterleaved = true,
      };
      actualOutput.validBits = actualOutput.bitDepth;

      info.sourceFormat = actualOutput;
      info.sourceFormat.isFloat = false;
      info.sourceFormat.bitDepth = kMp3PcmBitDepth;
      info.sourceFormat.validBits = kMp3PcmBitDepth;

      if (auto frameInfo = mpg123_frameinfo2{}; ::mpg123_info2(mh, &frameInfo) == MPG123_OK)
      {
        info.sourceFormat.sampleRate = static_cast<std::uint32_t>(frameInfo.rate);
        info.sourceFormat.channels = channelCountFromFrameMode(frameInfo.mode);
      }

      info.outputFormat = actualOutput;
      info.isLossy = true;
      return {};
    }
  };

  Mp3DecoderSession::Mp3DecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  Mp3DecoderSession::~Mp3DecoderSession() = default;

  Result<> Mp3DecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    if (auto const configureResult = _implPtr->configureOutputFormat(); !configureResult)
    {
      return std::unexpected{configureResult.error()};
    }

    if (auto const mapResult = _implPtr->mappedFile.map(filePath); !mapResult)
    {
      return std::unexpected{mapResult.error()};
    }

    _implPtr->currentOffset = 0;
    _implPtr->eof = false;
    _implPtr->nextFrameIndex = 0;

    ::mpg123_replace_reader_handle(_implPtr->mh, Impl::readCb, Impl::lseekCb, nullptr);

    if (::mpg123_open_handle(_implPtr->mh, _implPtr.get()) != MPG123_OK)
    {
      return makeError(Error::Code::InitFailed, "Failed to open MP3 handle");
    }

    // Scan for accurate length (especially for VBR)
    ::mpg123_scan(_implPtr->mh);

    if (auto const formatResult = _implPtr->refreshStreamInfo(); !formatResult)
    {
      return std::unexpected{formatResult.error()};
    }

    if (_implPtr->requestedOutput.sampleRate != 0 &&
        _implPtr->requestedOutput.sampleRate != _implPtr->info.outputFormat.sampleRate)
    {
      return makeError(Error::Code::NotSupported, "MP3 sample rate conversion is not supported");
    }

    if (_implPtr->requestedOutput.channels != 0 &&
        _implPtr->requestedOutput.channels != _implPtr->info.outputFormat.channels)
    {
      return makeError(Error::Code::NotSupported, "MP3 channel remapping is not supported");
    }

    // Estimate duration
    if (off_t const samples = ::mpg123_length(_implPtr->mh); samples > 0 && _implPtr->info.outputFormat.sampleRate > 0)
    {
      _implPtr->info.durationMs = static_cast<std::uint32_t>(static_cast<double>(samples) /
                                                             _implPtr->info.outputFormat.sampleRate * kMsPerSecond);
    }

    _implPtr->decodeBuffer.resize(::mpg123_outblock(_implPtr->mh));

    return {};
  }

  void Mp3DecoderSession::close()
  {
    if (_implPtr->mh != nullptr)
    {
      ::mpg123_close(_implPtr->mh);
    }

    _implPtr->mappedFile.unmap();
    _implPtr->eof = false;
  }

  Result<> Mp3DecoderSession::seek(std::uint32_t positionMs)
  {
    _implPtr->eof = false;

    if (positionMs > _implPtr->info.durationMs)
    {
      return makeError(Error::Code::SeekFailed, "Seek position out of bounds");
    }

    auto const sampleRate = _implPtr->info.sourceFormat.sampleRate;

    if (sampleRate == 0)
    {
      return makeError(Error::Code::SeekFailed, "Sample rate is 0");
    }

    auto const sampleOffset = static_cast<off_t>(static_cast<double>(positionMs) / kMsPerSecond * sampleRate);

    auto const actualOffset = ::mpg123_seek(_implPtr->mh, sampleOffset, SEEK_SET);

    if (actualOffset < 0)
    {
      return makeError(Error::Code::SeekFailed, "MP3 seek failed");
    }

    _implPtr->nextFrameIndex = static_cast<std::uint64_t>(actualOffset);
    return {};
  }

  void Mp3DecoderSession::flush()
  {
    // No explicit flush needed for mpg123 in this usage pattern
  }

  Result<PcmBlock> Mp3DecoderSession::readNextBlock()
  {
    if (_implPtr->eof)
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    size_t done = 0;

    while (true)
    {
      int const err = ::mpg123_read(_implPtr->mh,
                                    utility::layout::asLegacyPtr<unsigned char>(_implPtr->decodeBuffer.data()),
                                    _implPtr->decodeBuffer.size(),
                                    &done);

      if (err == MPG123_DONE)
      {
        _implPtr->eof = true;

        if (done == 0)
        {
          return PcmBlock{.bytes = {}, .endOfStream = true};
        }

        break;
      }

      if (err == MPG123_NEW_FORMAT)
      {
        auto const previousOutput = _implPtr->info.outputFormat;

        if (auto const formatResult = _implPtr->refreshStreamInfo(); !formatResult)
        {
          return std::unexpected{formatResult.error()};
        }

        if (!(_implPtr->info.outputFormat == previousOutput))
        {
          return makeError(Error::Code::NotSupported, "MP3 stream changed output format during playback");
        }

        if (done == 0)
        {
          continue;
        }

        break;
      }

      if (err != MPG123_OK)
      {
        return makeError(Error::Code::DecodeFailed, "MP3 decode error");
      }

      break;
    }

    if (done == 0)
    {
      _implPtr->eof = true;
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const bytesPerFrame =
      static_cast<std::uint32_t>(_implPtr->info.outputFormat.channels) * (_implPtr->info.outputFormat.bitDepth / 8U);

    if (bytesPerFrame == 0)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid MP3 output format");
    }

    std::uint32_t const frames = static_cast<std::uint32_t>(done / bytesPerFrame);
    std::uint64_t const currentFrameIndex = _implPtr->nextFrameIndex;
    _implPtr->nextFrameIndex += frames;

    return PcmBlock{.bytes = {_implPtr->decodeBuffer.data(), done},
                    .bitDepth = _implPtr->info.outputFormat.bitDepth,
                    .frames = frames,
                    .firstFrameIndex = currentFrameIndex,
                    .endOfStream = false};
  }

  DecodedStreamInfo Mp3DecoderSession::streamInfo() const
  {
    return _implPtr->info;
  }
} // namespace ao::audio
