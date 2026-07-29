// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/media/file/TestFile.h"

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace ao::media::file::test
{
  std::string_view RecordedContent::text(TextField const field) const
  {
    auto const iter = _texts.find(field);
    return iter == _texts.end() ? std::string_view{} : iter->second;
  }

  std::uint16_t RecordedContent::number(NumberField const field) const
  {
    auto const iter = _numbers.find(field);
    return iter == _numbers.end() ? 0 : iter->second;
  }

  VisitorSpy::VisitorSpy(RecordedContent& content)
    : _content{content}
  {
  }

  void VisitorSpy::text(TextField const field, std::string_view const value)
  {
    _content._texts.insert_or_assign(field, value);
    _content._events.push_back(
      {.kind = RecordedContent::CallbackKind::Text, .field = static_cast<std::uint8_t>(field)});
  }

  void VisitorSpy::number(NumberField const field, std::uint16_t const value)
  {
    _content._numbers.insert_or_assign(field, value);
    _content._events.push_back(
      {.kind = RecordedContent::CallbackKind::Number, .field = static_cast<std::uint8_t>(field)});
  }

  void VisitorSpy::codec(AudioCodec const value)
  {
    _content._codec = value;
    _content._events.push_back({.kind = RecordedContent::CallbackKind::Codec});
  }

  void VisitorSpy::duration(std::chrono::milliseconds const duration)
  {
    _content._duration = duration;
    _content._events.push_back({.kind = RecordedContent::CallbackKind::Duration});
  }

  void VisitorSpy::bitrate(Bitrate const value)
  {
    _content._bitrate = value;
    _content._events.push_back({.kind = RecordedContent::CallbackKind::Bitrate});
  }

  void VisitorSpy::sampleRate(SampleRate const value)
  {
    _content._sampleRate = value;
    _content._events.push_back({.kind = RecordedContent::CallbackKind::SampleRate});
  }

  void VisitorSpy::channels(Channels const value)
  {
    _content._channels = value;
    _content._events.push_back({.kind = RecordedContent::CallbackKind::Channels});
  }

  void VisitorSpy::bitDepth(BitDepth const value)
  {
    _content._bitDepth = value;
    _content._events.push_back({.kind = RecordedContent::CallbackKind::BitDepth});
  }

  void VisitorSpy::picture(PictureType const type, std::span<std::byte const> const bytes)
  {
    _content._pictures.push_back(RecordedContent::Picture{.type = type, .bytes = bytes});
    _content._events.push_back(
      {.kind = RecordedContent::CallbackKind::Picture, .field = static_cast<std::uint8_t>(type)});
  }

  struct TestFile::Impl final
  {
    explicit Impl(std::filesystem::path const& path)
      : fileResult{File::open(path)}
    {
    }

    Result<File> fileResult;
  };

  TestFile::TestFile(std::filesystem::path const& path)
    : _implPtr{std::make_unique<Impl>(path)}
  {
  }

  TestFile::~TestFile() = default;
  TestFile::TestFile(TestFile&&) noexcept = default;

  Result<RecordedContent> TestFile::readContent() const
  {
    if (!_implPtr->fileResult)
    {
      return std::unexpected{_implPtr->fileResult.error()};
    }

    auto content = RecordedContent{};
    auto visitor = VisitorSpy{content};

    if (auto const visitResult = _implPtr->fileResult->visit(visitor); !visitResult)
    {
      return std::unexpected{visitResult.error()};
    }

    return content;
  }

  Result<PayloadView> TestFile::audioPayload() const
  {
    if (!_implPtr->fileResult)
    {
      return std::unexpected{_implPtr->fileResult.error()};
    }

    return _implPtr->fileResult->audioPayload();
  }
} // namespace ao::media::file::test
