// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Mp3DecoderSession.h"

#include "../media/file/mpeg/ApeTag.h"
#include "../media/file/mpeg/Frame.h"
#include "../media/file/mpeg/FrameLayout.h"
#include "AudioTime.h"
#include "detail/DecoderError.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/MappedFileCursor.h"
#include "detail/Mpg123Runtime.h"
#include "detail/TimeConversion.h"
#include <ao/AudioCodec.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <mpg123.h>
#include <sys/types.h> // NOLINT(misc-include-cleaner) -- public owner for mpg123's native off_t API.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kMp3PcmBitDepth = 16;

    template<typename Action>
    decltype(auto) runOrAbort(std::string_view const context, Action&& action) noexcept
    {
      try
      {
        return std::forward<Action>(action)();
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), context);
      }
    }

    std::uint8_t channelCountFromMpg123(std::int32_t channels) noexcept
    {
      return (channels == MPG123_MONO) ? 1U : 2U;
    }

    std::uint8_t channelCountFromFrameMode(std::int32_t mode) noexcept
    {
      return (mode == MPG123_M_MONO) ? 1U : 2U;
    }

    constexpr std::size_t kId3v1ByteCount = 128;
    constexpr std::uint32_t kApeReadOnly = 0x1U;
    constexpr std::size_t kMaxMp3TailBytes = std::size_t{32} * 1024U * 1024U;
    constexpr std::size_t kApeDescriptorBytes = 32;
    constexpr std::size_t kApeFlagsOffset = 20;
    constexpr std::size_t kLyricsEndBytes = 9;
    constexpr std::uint32_t kApeVersion = 2000;
    constexpr std::uint32_t kApeHeaderPresent = 0x80000000U;
    constexpr std::uint32_t kApeIsHeader = 0x20000000U;

    bool hasMp3TagMarker(std::span<std::byte const> bytes, std::string_view marker) noexcept
    {
      return bytes.size() >= marker.size() && std::memcmp(bytes.data(), marker.data(), marker.size()) == 0;
    }

    std::uint32_t mp3TagU32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return std::to_integer<std::uint32_t>(bytes[offset]) | (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
             (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
             (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
    }

    bool hasApeTagItems(std::span<std::byte const> items, std::uint32_t count) noexcept
    {
      constexpr std::size_t kMinItemBytes = 11;
      constexpr std::size_t kMaxKeyBytes = 255;
      constexpr std::uint32_t kItemFlagsMask = 0x7U;
      constexpr std::uint32_t kItemTypeMask = 0x6U;

      for (std::uint32_t index = 0; index < count; ++index)
      {
        // Value length, flags, at least two key bytes and its terminator.
        if (items.size() < kMinItemBytes)
        {
          return false;
        }

        auto const valueBytes = mp3TagU32(items, 0);

        if (auto const flags = mp3TagU32(items, 4);
            (flags & ~kItemFlagsMask) != 0 || (flags & kItemTypeMask) == kItemTypeMask)
        {
          return false;
        }

        items = items.subspan(8);
        std::size_t keyBytes = 0;

        while (keyBytes < items.size() && items[keyBytes] != std::byte{0})
        {
          auto const character = std::to_integer<std::uint8_t>(items[keyBytes]);

          if (keyBytes == kMaxKeyBytes || character < ' ' || character > '~' || character == '=')
          {
            return false;
          }

          ++keyBytes;
        }

        if (keyBytes < 2 || keyBytes == items.size())
        {
          return false;
        }

        items = items.subspan(keyBytes + 1);

        if (valueBytes > items.size())
        {
          return false;
        }

        items = items.subspan(valueBytes);
      }

      return items.empty();
    }

    std::optional<std::size_t> apeTagStart(std::span<std::byte const> bytes)
    {
      auto const tagStartRes = media::file::mpeg::readApeTagStart(bytes);

      if (!tagStartRes)
      {
        return std::nullopt;
      }

      auto const footer = bytes.last(kApeDescriptorBytes);
      auto const tagBytes = mp3TagU32(footer, 12);
      auto const flags = mp3TagU32(footer, kApeFlagsOffset);

      if (mp3TagU32(footer, 8) != kApeVersion || tagBytes > kMaxMp3TailBytes ||
          (flags & ~(kApeHeaderPresent | kApeReadOnly)) != 0 ||
          !std::ranges::all_of(footer.last(8), [](std::byte val) { return val == std::byte{0}; }))
      {
        return std::nullopt;
      }

      if ((flags & kApeHeaderPresent) != 0)
      {
        auto const header = bytes.subspan(*tagStartRes, kApeDescriptorBytes);

        if (!std::ranges::equal(header.first(kApeFlagsOffset), footer.first(kApeFlagsOffset)) ||
            mp3TagU32(header, kApeFlagsOffset) != (flags | kApeIsHeader) ||
            !std::ranges::all_of(header.last(8), [](std::byte val) { return val == std::byte{0}; }))
        {
          return std::nullopt;
        }
      }

      auto const items = bytes.subspan(bytes.size() - tagBytes, tagBytes - kApeDescriptorBytes);
      return hasApeTagItems(items, mp3TagU32(footer, 16)) ? std::optional<std::size_t>{*tagStartRes} : std::nullopt;
    }

    std::optional<std::size_t> mp3TagDecimal(std::span<std::byte const> bytes) noexcept
    {
      constexpr std::size_t kDecimalBase = 10;
      std::size_t value = 0;

      for (auto const byte : bytes)
      {
        auto const digit = std::to_integer<std::uint8_t>(byte);

        if (digit < '0' || digit > '9')
        {
          return std::nullopt;
        }

        value = (value * kDecimalBase) + (digit - '0');
      }

      return value;
    }

    std::optional<std::size_t> lyricsTagStart(std::span<std::byte const> bytes) noexcept
    {
      constexpr std::size_t kLyricsTrailerBytes = 15;
      constexpr std::size_t kLyricsHeaderBytes = 11;

      if (bytes.size() < kLyricsTrailerBytes)
      {
        return std::nullopt;
      }

      auto const optSize = mp3TagDecimal(bytes.subspan(bytes.size() - kLyricsTrailerBytes, 6));

      if (!optSize || *optSize < kLyricsHeaderBytes || *optSize > bytes.size() - kLyricsTrailerBytes)
      {
        return std::nullopt;
      }

      auto const start = bytes.size() - kLyricsTrailerBytes - *optSize;
      auto fields = bytes.subspan(start, *optSize);

      if (!hasMp3TagMarker(fields, "LYRICSBEGIN"))
      {
        return std::nullopt;
      }

      fields = fields.subspan(kLyricsHeaderBytes);

      if (fields.empty())
      {
        return std::nullopt;
      }

      while (!fields.empty())
      {
        if (fields.size() < 8 ||
            !std::ranges::all_of(
              fields.first(3), [](std::byte val) { return val >= std::byte{'A'} && val <= std::byte{'Z'}; }))
        {
          return std::nullopt;
        }

        auto const optFieldSize = mp3TagDecimal(fields.subspan(3, 5));

        if (!optFieldSize || *optFieldSize > fields.size() - 8)
        {
          return std::nullopt;
        }

        fields = fields.subspan(8 + *optFieldSize);
      }

      return start;
    }

    std::optional<std::size_t> mp3AudioByteCount(std::span<std::byte const> bytes)
    {
      std::size_t end = bytes.size();
      bool const hasId3v1 = end >= kId3v1ByteCount && hasMp3TagMarker(bytes.last(kId3v1ByteCount), "TAG");

      if (hasId3v1)
      {
        end -= kId3v1ByteCount;
      }

      if (end >= kApeDescriptorBytes && hasMp3TagMarker(bytes.subspan(end - kApeDescriptorBytes), "APETAGEX"))
      {
        auto const optStart = apeTagStart(bytes.first(end));

        if (!optStart)
        {
          return std::nullopt;
        }

        end = *optStart;
      }

      if (end >= kLyricsEndBytes && hasMp3TagMarker(bytes.subspan(end - kLyricsEndBytes), "LYRICS200"))
      {
        auto const optStart = hasId3v1 ? lyricsTagStart(bytes.first(end)) : std::nullopt;

        if (!optStart)
        {
          return std::nullopt;
        }

        end = *optStart;
      }

      // Repeated or misordered declarations are not repaired by a valid outer tag.
      if ((end >= kId3v1ByteCount && hasMp3TagMarker(bytes.subspan(end - kId3v1ByteCount), "TAG")) ||
          (end >= kApeDescriptorBytes && hasMp3TagMarker(bytes.subspan(end - kApeDescriptorBytes), "APETAGEX")) ||
          (end >= kLyricsEndBytes && hasMp3TagMarker(bytes.subspan(end - kLyricsEndBytes), "LYRICS200")))
      {
        return std::nullopt;
      }

      return bytes.size() - end <= kMaxMp3TailBytes ? std::optional<std::size_t>{end} : std::nullopt;
    }

    std::string mpg123ErrorMessage(mpg123_handle* handle, int error)
    {
      char const* const detail = error == MPG123_ERR ? ::mpg123_strerror(handle) : ::mpg123_plain_strerror(error);
      return detail != nullptr ? std::string{detail} : std::string{"Unknown mpg123 error"};
    }
  } // namespace

  struct Mp3DecoderSession::Impl final
  {
    detail::DecoderOutputAdapter outputAdapter;
    detail::Mpg123EnvironmentGuard mpg123Environment;
    mpg123_handle* mh = nullptr;
    detail::MappedFileCursor fileCursor;
    std::optional<std::size_t> optAudioByteCount;
    DecodedStreamInfo info;
    std::vector<std::byte> decodeBuffer;
    std::uint64_t nextFrameIndex = 0;
    std::optional<Error> optTerminalError;
    bool eof = false;
    int initErr = MPG123_OK;

    Impl(std::optional<SampleEncoding> optOutputEncoding)
      : outputAdapter{optOutputEncoding}
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

    static mpg123_ssize_t readCb(void* handle, void* buf, size_t sz)
    {
      auto* self = static_cast<Impl*>(handle);
      auto const end = self->optAudioByteCount.value_or(self->fileCursor.size());
      auto const position = self->fileCursor.position();

      if (position >= end)
      {
        return 0;
      }

      auto const remaining = static_cast<std::size_t>(end - position);
      auto const count = self->fileCursor.read({static_cast<std::byte*>(buf), std::min(sz, remaining)});
      return static_cast<mpg123_ssize_t>(count);
    }

    // Darwin exposes off_t through public <sys/types.h>, backed by an SDK-private declaration header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    static off_t lseekCb(void* handle, off_t offset, std::int32_t whence)
    {
      auto* self = static_cast<Impl*>(handle);
      std::uint64_t const end = self->optAudioByteCount.value_or(self->fileCursor.size());
      std::uint64_t base = 0;

      switch (whence)
      {
        case SEEK_SET: break;
        case SEEK_CUR: base = self->fileCursor.position(); break;
        case SEEK_END: base = end; break;
        default: return -1;
      }

      std::uint64_t target = 0;

      if (offset < 0)
      {
        auto const magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1U;

        if (magnitude > base)
        {
          return -1;
        }

        target = base - magnitude;
      }
      else
      {
        auto const positiveOffset = static_cast<std::uint64_t>(offset);

        if (base > end || positiveOffset > end - base)
        {
          return -1;
        }

        target = base + positiveOffset;
      }

      if (target > end || target > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
      {
        return -1;
      }

      auto const res = self->fileCursor.seek(static_cast<std::int64_t>(target), detail::SeekOrigin::Begin);
      return res ? static_cast<off_t>(*res) : -1;
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

      // Decode one stable native format. The shared output adapter performs all
      // precision-preserving container expansion after stream inspection.
      auto constexpr kEncoding = MPG123_ENC_SIGNED_16; // NOLINT(misc-include-cleaner)

      if (int const err = ::mpg123_format2(mh, 0, MPG123_MONO | MPG123_STEREO, kEncoding); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::NotSupported, "Unsupported MP3 output format: " + mpg123ErrorMessage(mh, err));
      }
    }

    void scanNativeStream()
    {
      if (auto const res = fileCursor.seek(0, detail::SeekOrigin::Begin); !res)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to reset MP3 input: " + res.error().message);
      }

      if (int const err = ::mpg123_open_handle(mh, this); err != MPG123_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to open MP3 handle: " + mpg123ErrorMessage(mh, err));
      }

      if (int const err = ::mpg123_scan(mh); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::DecodeFailed, "Failed to scan MP3 stream: " + mpg123ErrorMessage(mh, err));
      }

      if (auto const actualOffset = ::mpg123_seek(mh, 0, SEEK_SET); actualOffset != 0)
      {
        int const error = actualOffset < 0 ? static_cast<std::int32_t>(actualOffset) : MPG123_ERR;
        detail::throwDecoderError(
          Error::Code::DecodeFailed, "Failed to restore MP3 stream start after scan: " + mpg123ErrorMessage(mh, error));
      }
    }

    bool hasIndexedAudioEnd(std::size_t byteCount) const
    {
      std::int64_t* offsets = nullptr;
      std::int64_t step = 0;
      std::size_t count = 0;

      if (::mpg123_index64(mh, &offsets, &step, &count) != MPG123_OK || offsets == nullptr || step != 1)
      {
        return false;
      }

      auto const bytes = fileCursor.bytes();

      for (std::size_t index = 0; index < count; ++index)
      {
        if (offsets[index] < 0 || std::cmp_greater_equal(offsets[index], byteCount))
        {
          break;
        }

        auto const offset = static_cast<std::size_t>(offsets[index]);

        if (bytes.size() - offset < sizeof(media::file::mpeg::FrameLayout))
        {
          return false;
        }

        auto const frame = media::file::mpeg::FrameView{bytes.data() + offset, bytes.size() - offset};

        if (frame.isValid() && frame.length() == byteCount - offset)
        {
          return true;
        }
      }

      return false;
    }

    SignalFormat inspectSourceFormat() const
    {
      long rate = 0;
      int channels = 0;
      int encoding = 0;

      if (int const err = ::mpg123_getformat2(mh, &rate, &channels, &encoding, 0); err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::DecodeFailed, "Failed to get MP3 format: " + mpg123ErrorMessage(mh, err));
      }

      if (encoding != MPG123_ENC_SIGNED_16) // NOLINT(misc-include-cleaner)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "MP3 decoder changed its native PCM encoding");
      }

      auto sourceFormat = SignalFormat{.sampleRate = static_cast<std::uint32_t>(rate),
                                       .channels = channelCountFromMpg123(channels),
                                       .precisionBits = kMp3PcmBitDepth,
                                       .sampleKind = SampleKind::Integer};

      if (auto frameInfo = mpg123_frameinfo2{}; ::mpg123_info2(mh, &frameInfo) == MPG123_OK)
      {
        if (std::cmp_not_equal(sourceFormat.sampleRate, frameInfo.rate) ||
            sourceFormat.channels != channelCountFromFrameMode(frameInfo.mode))
        {
          detail::throwDecoderError(Error::Code::NotSupported, "MP3 frame and native PCM geometry disagree");
        }

        switch (frameInfo.mode)
        {
          case MPG123_M_MONO: sourceFormat.speakerMask = 0x4; break;
          case MPG123_M_STEREO:
          case MPG123_M_JOINT: sourceFormat.speakerMask = 0x3; break;
          // Dual-channel carries independent mono programs, not an assigned FL/FR pair.
          default: break;
        }
      }
      else
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "MP3 frame mode is unavailable");
      }

      return sourceFormat;
    }

    std::uint32_t inspectStreamSpeakerMask(SignalFormat const& sourceFormat) const
    {
      std::int64_t* offsets = nullptr;
      std::int64_t step = 0;
      std::size_t count = 0;

      // A sampled or partial seek index cannot prove the roles of the omitted frames.
      if (auto const frameCount = ::mpg123_framelength64(mh);
          !optAudioByteCount || sourceFormat.speakerMask == 0 || frameCount <= 0 ||
          ::mpg123_index64(mh, &offsets, &step, &count) != MPG123_OK || offsets == nullptr || step != 1 ||
          std::cmp_not_equal(count, frameCount))
      {
        return 0;
      }

      auto const bytes = fileCursor.bytes().first(*optAudioByteCount);
      std::size_t nextOffset = 0;

      for (std::size_t index = 0; index < count; ++index)
      {
        if (offsets[index] < 0 || std::cmp_greater(offsets[index], bytes.size()))
        {
          return 0;
        }

        auto const offset = static_cast<std::size_t>(offsets[index]);
        auto const remaining = bytes.subspan(offset);

        if ((index != 0 && offset != nextOffset) || remaining.size() < sizeof(media::file::mpeg::FrameLayout))
        {
          return 0;
        }

        auto const frame = media::file::mpeg::FrameView{remaining.data(), remaining.size()};

        if (!frame.isValid())
        {
          return 0;
        }

        auto const length = frame.length();

        if (length < sizeof(media::file::mpeg::FrameLayout) || length > remaining.size() ||
            frame.sampleRate() != sourceFormat.sampleRate || frame.channels() != sourceFormat.channels ||
            frame.layout().channelMode() == media::file::mpeg::ChannelMode::DualChannel)
        {
          return 0;
        }

        nextOffset = offset + length;
      }

      return nextOffset == bytes.size() ? sourceFormat.speakerMask : 0;
    }

    void configureStreamInfo()
    {
      info.sourceFormat = inspectSourceFormat();
      info.sourceFormat.speakerMask = inspectStreamSpeakerMask(info.sourceFormat);
      info.sourceChannelLayout.origin =
        info.sourceFormat.speakerMask != 0 ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown;
      auto const configuredRes = outputAdapter.configure(info.sourceFormat, SampleEncoding::Signed16Le);

      if (!configuredRes)
      {
        detail::throwDecoderError(configuredRes.error());
      }

      info.outputFormat = *configuredRes;
      info.isLossy = true;
      info.codec = AudioCodec::Mp3;
    }

    Result<PcmBlock> readNextBlock();
  };

  Result<PcmBlock> Mp3DecoderSession::Impl::readNextBlock()
  {
    auto* const impl = this;

    try
    {
      if (impl->optTerminalError)
      {
        return std::unexpected{*impl->optTerminalError};
      }

      if (!impl->fileCursor.isOpen() || impl->eof)
      {
        return PcmBlock{.endOfStream = true};
      }

      std::size_t done = 0;
      unsigned char* audio = nullptr;

      while (true)
      {
        // A read-sized buffer can straddle MPEG frames after gapless trimming.
        // Inspect each decoded frame so a same-channel role change cannot hide inside a block.
        int const err = ::mpg123_decode_frame(impl->mh, nullptr, &audio, &done);

        if (err == MPG123_DONE)
        {
          impl->eof = true;

          if (done == 0)
          {
            return PcmBlock{.endOfStream = true};
          }
        }
        else if (err != MPG123_OK && err != MPG123_NEW_FORMAT)
        {
          detail::throwDecoderError(
            Error::Code::DecodeFailed, "MP3 decode error: " + mpg123ErrorMessage(impl->mh, err));
        }

        if (err == MPG123_NEW_FORMAT || done != 0)
        {
          auto sourceFormat = impl->inspectSourceFormat();

          if (impl->info.sourceFormat.speakerMask == 0)
          {
            // Incomplete or mixed initial evidence stays unassigned, without weakening geometry checks.
            sourceFormat.speakerMask = 0;
          }

          if (!(sourceFormat == impl->info.sourceFormat))
          {
            detail::throwDecoderError(Error::Code::NotSupported, "MP3 stream changed source format during playback");
          }
        }

        if (done != 0)
        {
          break;
        }
      }

      if (audio == nullptr)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "MP3 decoder returned no audio buffer");
      }

      constexpr std::uint32_t kNativeBytesPerSample = 2;
      auto const bytesPerFrame = static_cast<std::uint32_t>(impl->info.outputFormat.channels) * kNativeBytesPerSample;

      if (bytesPerFrame == 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Invalid MP3 output format");
      }

      std::uint32_t const frames = static_cast<std::uint32_t>(done / bytesPerFrame);

      if ((done % bytesPerFrame) != 0 || frames == 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Misaligned MP3 decoder output");
      }

      std::uint64_t const currentFrameIndex = impl->nextFrameIndex;
      impl->nextFrameIndex += frames;

      // mpg123 already applies in-frame seek skips and gapless trimming to this returned span.
      auto nativeBytes = std::as_bytes(std::span<unsigned char const>{audio, done});

      if (impl->info.outputFormat.encoding == SampleEncoding::Signed16Le)
      {
        // A seek can reallocate mpg123's buffer before the next public read. Preserve PcmBlock's lifetime.
        impl->decodeBuffer.assign(nativeBytes.begin(), nativeBytes.end());
        nativeBytes = impl->decodeBuffer;
      }

      auto convertedRes = impl->outputAdapter.convert(nativeBytes);

      if (!convertedRes)
      {
        detail::throwDecoderError(convertedRes.error());
      }

      return PcmBlock{
        .bytes = *convertedRes, .frames = frames, .firstFrameIndex = currentFrameIndex, .endOfStream = false};
    }
    catch (detail::DecoderException const& ex)
    {
      impl->optTerminalError = ex.error();
      return std::unexpected{ex.error()};
    }
  }

  Mp3DecoderSession::Mp3DecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
  {
  }

  Mp3DecoderSession::~Mp3DecoderSession() = default;

  Result<> Mp3DecoderSession::initialize(std::filesystem::path const& filePath) noexcept
  {
    try
    {
      _implPtr->configureOutputFormat();

      if (auto const res = _implPtr->fileCursor.open(filePath); !res)
      {
        detail::throwDecoderError(res.error());
      }

      _implPtr->eof = false;
      _implPtr->nextFrameIndex = 0;

      if (int const err = ::mpg123_replace_reader_handle(_implPtr->mh, Impl::readCb, Impl::lseekCb, nullptr);
          err != MPG123_OK)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, "Failed to configure MP3 input callbacks: " + mpg123ErrorMessage(_implPtr->mh, err));
      }

      // 128 KiB growth quanta reduce reallocations; completeness is checked before any role is published.
      constexpr long kFrameIndexGrowth = -16384;
      std::ignore = ::mpg123_param(_implPtr->mh, MPG123_INDEX_SIZE, kFrameIndexGrowth, 0.0);

      _implPtr->scanNativeStream();
      auto const fileBytes = _implPtr->fileCursor.bytes();

      if (auto const optByteCount = mp3AudioByteCount(fileBytes);
          optByteCount && (*optByteCount == fileBytes.size() || _implPtr->hasIndexedAudioEnd(*optByteCount)))
      {
        _implPtr->optAudioByteCount = optByteCount;

        // Native ID3v1 handling consumes the whole aligned tag. Reuse its checked scan
        // with dynamically bounded callbacks instead of repeating prefix lookahead.
        bool const hasOnlyId3v1 = fileBytes.size() - *optByteCount == kId3v1ByteCount &&
                                  hasMp3TagMarker(fileBytes.last(kId3v1ByteCount), "TAG");

        if (*optByteCount < fileBytes.size() && !hasOnlyId3v1)
        {
          // APE and Lyrics values can pollute the first scan's index and PCM extent.
          _implPtr->scanNativeStream();
        }
      }

      _implPtr->configureStreamInfo();

      if (off_t const samples = ::mpg123_length(_implPtr->mh);
          samples > 0 && _implPtr->info.outputFormat.sampleRate > 0)
      {
        _implPtr->info.duration =
          detail::convertToDuration(static_cast<std::uint64_t>(samples), _implPtr->info.outputFormat.sampleRate);
      }

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<> Mp3DecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    return runOrAbort(
      "MP3 decoder seek",
      [this, offset] -> Result<>
      {
        if (!_implPtr->fileCursor.isOpen())
        {
          return makeError(Error::Code::SeekFailed, "MP3 decoder is not open");
        }

        if (offset < std::chrono::milliseconds{0} ||
            (_implPtr->info.duration > std::chrono::milliseconds{0} && offset > _implPtr->info.duration))
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
      });
  }

  void Mp3DecoderSession::flush() noexcept
  {
    // No explicit flush needed for mpg123 in this usage pattern
  }

  Result<PcmBlock> Mp3DecoderSession::readNextBlock() noexcept
  {
    return runOrAbort("MP3 decoder read", [this] { return _implPtr->readNextBlock(); });
  }

  DecodedStreamInfo Mp3DecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
