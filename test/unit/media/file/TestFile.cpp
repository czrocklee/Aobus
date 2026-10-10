// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/media/file/TestFile.h"

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    record({.kind = RecordedContent::CallbackKind::Text, .field = static_cast<std::uint8_t>(field)});
  }

  void VisitorSpy::number(NumberField const field, std::uint16_t const value)
  {
    _content._numbers.insert_or_assign(field, value);
    record({.kind = RecordedContent::CallbackKind::Number, .field = static_cast<std::uint8_t>(field)});
  }

  void VisitorSpy::codec(AudioCodec const value)
  {
    _content._codec = value;
    record({.kind = RecordedContent::CallbackKind::Codec});
  }

  void VisitorSpy::duration(std::chrono::milliseconds const duration)
  {
    _content._duration = duration;
    record({.kind = RecordedContent::CallbackKind::Duration});
  }

  void VisitorSpy::bitrate(Bitrate const value)
  {
    _content._bitrate = value;
    record({.kind = RecordedContent::CallbackKind::Bitrate});
  }

  void VisitorSpy::sampleRate(SampleRate const value)
  {
    _content._sampleRate = value;
    record({.kind = RecordedContent::CallbackKind::SampleRate});
  }

  void VisitorSpy::channels(Channels const value)
  {
    _content._channels = value;
    record({.kind = RecordedContent::CallbackKind::Channels});
  }

  void VisitorSpy::bitDepth(BitDepth const value)
  {
    _content._bitDepth = value;
    record({.kind = RecordedContent::CallbackKind::BitDepth});
  }

  void VisitorSpy::picture(PictureType const type, std::span<std::byte const> const bytes)
  {
    _content._pictures.push_back(RecordedContent::Picture{.type = type, .bytes = bytes});
    record({.kind = RecordedContent::CallbackKind::Picture, .field = static_cast<std::uint8_t>(type)});
  }

  void VisitorSpy::visitCredits(std::span<CreditView const> const entries)
  {
    REQUIRE_FALSE(entries.empty());

    // Copy synchronously: neither the span nor its strings escape this callback.
    auto owned = std::vector<RecordedContent::Credit>{};
    owned.reserve(entries.size());

    for (auto const& entry : entries)
    {
      owned.push_back({.name = std::string{entry.name}, .kind = entry.kind, .role = std::string{entry.role}});
    }

    _content._credits = std::move(owned);
    record({.kind = RecordedContent::CallbackKind::Credits});
  }

  void VisitorSpy::record(RecordedContent::CallbackEvent event)
  {
    if (!_content._events.empty())
    {
      // Credits is the one final delivery: no scalar, picture, or second list follows.
      CHECK(_content._events.back().kind != RecordedContent::CallbackKind::Credits);
    }

    _content._events.push_back(event);
  }

  struct TestFile::Impl final
  {
    explicit Impl(std::filesystem::path const& path)
      : fileRes{File::open(path)}
    {
    }

    Result<File> fileRes;
  };

  TestFile::TestFile(std::filesystem::path const& path)
    : _implPtr{std::make_unique<Impl>(path)}
  {
  }

  TestFile::~TestFile() = default;
  TestFile::TestFile(TestFile&&) noexcept = default;

  Result<RecordedContent> TestFile::readContent() const
  {
    if (!_implPtr->fileRes)
    {
      return std::unexpected{_implPtr->fileRes.error()};
    }

    auto content = RecordedContent{};
    auto visitor = VisitorSpy{content};

    if (auto const visitRes = _implPtr->fileRes->visit(visitor); !visitRes)
    {
      return std::unexpected{visitRes.error()};
    }

    return content;
  }

  Result<PayloadView> TestFile::audioPayload() const
  {
    if (!_implPtr->fileRes)
    {
      return std::unexpected{_implPtr->fileRes.error()};
    }

    return _implPtr->fileRes->audioPayload();
  }

  std::vector<std::byte> requireSoleEmbeddedPicture(std::filesystem::path const& path)
  {
    auto const file = TestFile{path};
    auto const contentRes = file.readContent();
    REQUIRE(contentRes);
    REQUIRE(contentRes->pictures().size() == 1);
    auto const bytes = contentRes->pictures().front().bytes;
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
  }
} // namespace ao::media::file::test
