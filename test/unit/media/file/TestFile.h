// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
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
      CallbackKind kind = CallbackKind::Text;
      std::uint8_t field = 0;

      bool operator==(CallbackEvent const&) const = default;
    };

    struct Picture final
    {
      PictureType type = PictureType::Other;
      std::span<std::byte const> bytes;
    };

    std::string_view text(TextField field) const;
    std::uint16_t number(NumberField field) const;

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
    explicit VisitorSpy(RecordedContent& content);

    void text(TextField field, std::string_view value) override;
    void number(NumberField field, std::uint16_t value) override;
    void codec(AudioCodec value) override;
    void duration(std::chrono::milliseconds duration) override;
    void bitrate(Bitrate value) override;
    void sampleRate(SampleRate value) override;
    void channels(Channels value) override;
    void bitDepth(BitDepth value) override;
    void picture(PictureType type, std::span<std::byte const> bytes) override;

  private:
    RecordedContent& _content;
  };

  /**
   * @brief Copies out the one picture carried by the media file at @p path.
   *
   * A test that names a cover by digest needs the exact payload the reader hands
   * out; container framing makes that impossible to guess from the fixture file.
   */
  std::vector<std::byte> requireSoleEmbeddedPicture(std::filesystem::path const& path);

  /** Test facade preserving the old parser-test shape while exercising only the public API. */
  class TestFile final
  {
  public:
    explicit TestFile(std::filesystem::path const& path);
    ~TestFile();

    TestFile(TestFile const&) = delete;
    TestFile& operator=(TestFile const&) = delete;
    TestFile(TestFile&&) noexcept;
    TestFile& operator=(TestFile&&) = delete;

    Result<RecordedContent> readContent() const;
    Result<PayloadView> audioPayload() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::media::file::test
