// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::file::test
{
  class RecordedContent final
  {
  public:
    enum class CallbackKind : std::uint8_t
    {
      Text,
      Number,
      Codec,
      Duration,
      Bitrate,
      SampleRate,
      Channels,
      BitDepth,
      Picture,
    };

    struct CallbackEvent final
    {
      CallbackKind kind;
      std::uint8_t field = 0;

      bool operator==(CallbackEvent const&) const = default;
    };

    struct Picture final
    {
      PictureType type = PictureType::Other;
      std::span<std::byte const> bytes;
    };

    std::string_view text(TextField field) const
    {
      auto const iter = _texts.find(field);
      return iter == _texts.end() ? std::string_view{} : iter->second;
    }

    std::uint16_t number(NumberField field) const
    {
      auto const iter = _numbers.find(field);
      return iter == _numbers.end() ? 0 : iter->second;
    }

    AudioCodec codec() const noexcept { return _codec; }
    std::chrono::milliseconds duration() const noexcept { return _duration; }
    Bitrate bitrate() const noexcept { return _bitrate; }
    SampleRate sampleRate() const noexcept { return _sampleRate; }
    Channels channels() const noexcept { return _channels; }
    BitDepth bitDepth() const noexcept { return _bitDepth; }
    std::vector<Picture> const& pictures() const noexcept { return _pictures; }
    std::vector<CallbackEvent> const& events() const noexcept { return _events; }
    std::size_t callCount() const noexcept { return _events.size(); }

  private:
    friend class VisitorSpy;

    std::map<TextField, std::string_view> _texts;
    std::map<NumberField, std::uint16_t> _numbers;
    AudioCodec _codec = AudioCodec::Unknown;
    std::chrono::milliseconds _duration{0};
    Bitrate _bitrate{};
    SampleRate _sampleRate{};
    Channels _channels{};
    BitDepth _bitDepth{};
    std::vector<Picture> _pictures;
    std::vector<CallbackEvent> _events;
  };

  class VisitorSpy final : public Visitor
  {
  public:
    explicit VisitorSpy(RecordedContent& content)
      : _content{content}
    {
    }

    void text(TextField field, std::string_view value) override
    {
      _content._texts.insert_or_assign(field, value);
      _content._events.push_back(
        {.kind = RecordedContent::CallbackKind::Text, .field = static_cast<std::uint8_t>(field)});
    }

    void number(NumberField field, std::uint16_t value) override
    {
      _content._numbers.insert_or_assign(field, value);
      _content._events.push_back(
        {.kind = RecordedContent::CallbackKind::Number, .field = static_cast<std::uint8_t>(field)});
    }

    void codec(AudioCodec value) override
    {
      _content._codec = value;
      _content._events.push_back({.kind = RecordedContent::CallbackKind::Codec});
    }

    void duration(std::chrono::milliseconds duration) override
    {
      _content._duration = duration;
      _content._events.push_back({.kind = RecordedContent::CallbackKind::Duration});
    }

    void bitrate(Bitrate value) override
    {
      _content._bitrate = value;
      _content._events.push_back({.kind = RecordedContent::CallbackKind::Bitrate});
    }

    void sampleRate(SampleRate value) override
    {
      _content._sampleRate = value;
      _content._events.push_back({.kind = RecordedContent::CallbackKind::SampleRate});
    }

    void channels(Channels value) override
    {
      _content._channels = value;
      _content._events.push_back({.kind = RecordedContent::CallbackKind::Channels});
    }

    void bitDepth(BitDepth value) override
    {
      _content._bitDepth = value;
      _content._events.push_back({.kind = RecordedContent::CallbackKind::BitDepth});
    }

    void picture(PictureType type, std::span<std::byte const> bytes) override
    {
      _content._pictures.push_back(RecordedContent::Picture{.type = type, .bytes = bytes});
      _content._events.push_back(
        {.kind = RecordedContent::CallbackKind::Picture, .field = static_cast<std::uint8_t>(type)});
    }

  private:
    RecordedContent& _content;
  };

  /** Test facade preserving the old parser-test shape while exercising only the public API. */
  class TestFile final
  {
  public:
    explicit TestFile(std::filesystem::path const& path)
      : _fileResult{File::open(path)}
    {
    }

    Result<RecordedContent> readContent() const
    {
      if (!_fileResult)
      {
        return std::unexpected{_fileResult.error()};
      }

      auto content = RecordedContent{};
      auto visitor = VisitorSpy{content};

      if (auto const visitResult = _fileResult->visit(visitor); !visitResult)
      {
        return std::unexpected{visitResult.error()};
      }

      return content;
    }

    Result<PayloadView> audioPayload() const
    {
      if (!_fileResult)
      {
        return std::unexpected{_fileResult.error()};
      }

      return _fileResult->audioPayload();
    }

  private:
    Result<File> _fileResult;
  };
} // namespace ao::media::file::test
