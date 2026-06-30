// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/MappedFileCursor.h"
#include "detail/OutputFormatValidation.h"
#include "detail/TimeConversion.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/Mp3DecoderSession.h>
#include <ao/audio/detail/DecoderError.h>
#include <ao/audio/detail/Mpg123Runtime.h>
#include <ao/utility/ByteView.h>

#include <mpg123.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kMp3PcmBitDepth = 16;
    constexpr std::uint8_t kFloat32BitDepth = 32;

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

    std::string mpg123ErrorMessage(mpg123_handle* handle, int error)
    {
      char const* const detail = error == MPG123_ERR ? ::mpg123_strerror(handle) : ::mpg123_plain_strerror(error);
      return detail != nullptr ? std::string{detail} : std::string{"Unknown mpg123 error"};
    }
  } // namespace

  struct Mp3DecoderSession::Impl final
  {
    Format requestedOutput;
    detail::Mpg123EnvironmentGuard mpg123Environment;
    mpg123_handle* mh = nullptr;
    detail::MappedFileCursor fileCursor;
    DecodedStreamInfo info;
    std::vector<std::byte> decodeBuffer;
    std::uint64_t nextFrameIndex = 0;
    std::optional<Error> optTerminalError;
    bool eof = false;
    int initErr = MPG123_OK;

    Impl(Format const& output)
      : requestedOutput{output}
    {
      mh = ::mpg123_new(nullptr, &initErr);

      if (mh != nullptr)
      {
        ::mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
      }
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
      auto const count = self->fileCursor.read({static_cast<std::byte*>(buf), sz});
      return static_cast<ssize_t>(count);
    }

    static off_t lseekCb(void* handle, off_t offset, std::int32_t whence)
    {
      auto* self = static_cast<Impl*>(handle);
      auto origin = detail::SeekOrigin::Begin;

      switch (whence)
      {
        case SEEK_SET: origin = detail::SeekOrigin::Begin; break;
        case SEEK_CUR: origin = detail::SeekOrigin::Current; break;
        case SEEK_END: origin = detail::SeekOrigin::End; break;
        default: return -1;
      }

      auto const result = self->fileCursor.seek(offset, origin);

      if (!result || *result > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
      {
        return -1;
      }

      return static_cast<off_t>(*result);
    }

    void configureOutputFormat() const
    {
      if (mh == nullptr)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, std::string{"Failed to create MP3 handle: "} + ::mpg123_plain_strerror(initErr));
      }

      if (int const err = ::mpg123_format_none(mh); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, "Failed to reset MP3 output formats: " + mpg123ErrorMessage(mh, err));
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
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported MP3 output sample format");
      }

      if (requestedOutput.channels > 2)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported MP3 channel count");
      }

      if (int const err = ::mpg123_format2(mh, 0, MPG123_MONO | MPG123_STEREO, encoding); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::NotSupported, "Unsupported MP3 output format: " + mpg123ErrorMessage(mh, err));
      }
    }

    void refreshStreamInfo()
    {
      long rate = 0;
      int channels = 0;
      int encoding = 0;

      if (int const err = ::mpg123_getformat(mh, &rate, &channels, &encoding); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::DecodeFailed, "Failed to get MP3 format: " + mpg123ErrorMessage(mh, err));
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
      info.codec = AudioCodec::Mp3;
    }
  };

  Mp3DecoderSession::Mp3DecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  Mp3DecoderSession::~Mp3DecoderSession() = default;

  Result<> Mp3DecoderSession::openCodec(std::filesystem::path const& filePath)
  {
    try
    {
      _implPtr->configureOutputFormat();

      if (auto const result = _implPtr->fileCursor.open(filePath); !result)
      {
        detail::throwDecoderError(result.error());
      }

      _implPtr->eof = false;
      _implPtr->nextFrameIndex = 0;

      if (int const err = ::mpg123_replace_reader_handle(_implPtr->mh, Impl::readCb, Impl::lseekCb, nullptr);
          err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, "Failed to configure MP3 input callbacks: " + mpg123ErrorMessage(_implPtr->mh, err));
      }

      if (int const err = ::mpg123_open_handle(_implPtr->mh, _implPtr.get()); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, "Failed to open MP3 handle: " + mpg123ErrorMessage(_implPtr->mh, err));
      }

      ::mpg123_scan(_implPtr->mh);

      _implPtr->refreshStreamInfo();

      if (auto const result =
            detail::validateFixedOutputRequest(_implPtr->requestedOutput, _implPtr->info.outputFormat, "MP3");
          !result)
      {
        detail::throwDecoderError(result.error());
      }

      if (_implPtr->requestedOutput.validBits != 0 &&
          _implPtr->requestedOutput.validBits != _implPtr->info.outputFormat.validBits)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported MP3 output valid bits");
      }

      if (off_t const samples = ::mpg123_length(_implPtr->mh);
          samples > 0 && _implPtr->info.outputFormat.sampleRate > 0)
      {
        _implPtr->info.duration =
          detail::convertToDuration(static_cast<std::uint64_t>(samples), _implPtr->info.outputFormat.sampleRate);
      }

      auto const outputBlockSize = ::mpg123_outblock(_implPtr->mh);

      if (outputBlockSize == 0)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "MP3 decoder reported an empty output buffer");
      }

      _implPtr->decodeBuffer.resize(outputBlockSize);
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  void Mp3DecoderSession::close() noexcept
  {
    if (_implPtr->mh != nullptr)
    {
      ::mpg123_close(_implPtr->mh);
    }

    _implPtr->fileCursor.close();
    _implPtr->decodeBuffer.clear();
    _implPtr->nextFrameIndex = 0;
    _implPtr->optTerminalError.reset();
    _implPtr->eof = false;
    _implPtr->info = {};
  }

  Result<> Mp3DecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    if (!_implPtr->fileCursor.isOpen())
    {
      return makeError(Error::Code::SeekFailed, "MP3 decoder is not open");
    }

    if (offset > _implPtr->info.duration)
    {
      return makeError(Error::Code::SeekFailed, "Seek offset out of bounds");
    }

    auto const sampleRate = _implPtr->info.sourceFormat.sampleRate;

    if (sampleRate == 0)
    {
      return makeError(Error::Code::SeekFailed, "Sample rate is 0");
    }

    auto const sampleOffset = static_cast<off_t>(durationToSamples(offset, sampleRate));

    auto const actualOffset = ::mpg123_seek(_implPtr->mh, sampleOffset, SEEK_SET);

    if (actualOffset < 0)
    {
      return makeError(Error::Code::SeekFailed, "MP3 seek failed: " + mpg123ErrorMessage(_implPtr->mh, MPG123_ERR));
    }

    _implPtr->nextFrameIndex = static_cast<std::uint64_t>(actualOffset);
    _implPtr->optTerminalError.reset();
    _implPtr->eof = false;
    return {};
  }

  void Mp3DecoderSession::flush() noexcept
  {
    // No explicit flush needed for mpg123 in this usage pattern
  }

  Result<PcmBlock> Mp3DecoderSession::readNextBlock() noexcept
  {
    try
    {
      if (_implPtr->optTerminalError)
      {
        return std::unexpected{*_implPtr->optTerminalError};
      }

      if (!_implPtr->fileCursor.isOpen() || _implPtr->eof)
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
          auto const previousInfo = _implPtr->info;

          _implPtr->refreshStreamInfo();

          if (!(_implPtr->info.outputFormat == previousInfo.outputFormat))
          {
            _implPtr->info = previousInfo;
            detail::throwDecoderError(Error::Code::NotSupported, "MP3 stream changed output format during playback");
          }

          if (done == 0)
          {
            continue;
          }

          break;
        }

        if (err != MPG123_OK)
        {
          detail::throwDecoderError(
            Error::Code::DecodeFailed, "MP3 decode error: " + mpg123ErrorMessage(_implPtr->mh, err));
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
        detail::throwDecoderError(Error::Code::DecodeFailed, "Invalid MP3 output format");
      }

      std::uint32_t const frames = static_cast<std::uint32_t>(done / bytesPerFrame);

      if ((done % bytesPerFrame) != 0 || frames == 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Misaligned MP3 decoder output");
      }

      std::uint64_t const currentFrameIndex = _implPtr->nextFrameIndex;
      _implPtr->nextFrameIndex += frames;

      return PcmBlock{.bytes = {_implPtr->decodeBuffer.data(), done},
                      .bitDepth = _implPtr->info.outputFormat.bitDepth,
                      .frames = frames,
                      .firstFrameIndex = currentFrameIndex,
                      .endOfStream = false};
    }
    catch (detail::DecoderException const& ex)
    {
      _implPtr->optTerminalError = ex.error();
      return std::unexpected{ex.error()};
    }
  }

  DecodedStreamInfo Mp3DecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
