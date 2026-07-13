// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/file/TestFile.h"
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::media::file::test
{
  using namespace ao::test;

  static_assert(std::is_move_constructible_v<File>);
  static_assert(!std::is_move_assignable_v<File>);

  TEST_CASE("Media File - recognizes and opens supported extensions", "[media][unit][factory]")
  {
    constexpr auto kExtensions = std::to_array<std::string_view>({".mp3", ".m4a", ".flac", ".wav"});

    for (auto const extension : kExtensions)
    {
      CAPTURE(extension);
      auto const temp = TempFile{extension};
      CHECK(File::isSupported(temp.path));
      CHECK(File::open(temp.path));
    }
  }

  TEST_CASE("Media File - reports unsupported and inaccessible inputs", "[media][unit][factory]")
  {
    SECTION("unknown extension")
    {
      auto const temp = TempFile{".txt"};
      CHECK_FALSE(File::isSupported(temp.path));

      auto fileResult = File::open(temp.path);
      REQUIRE_FALSE(fileResult);
      CHECK(fileResult.error().code == Error::Code::NotSupported);
    }

    SECTION("missing supported file")
    {
      auto fileResult = File::open("/tmp/aobus-missing-file.mp3");
      REQUIRE_FALSE(fileResult);
      CHECK(fileResult.error().code == Error::Code::IoError);
    }
  }

  TEST_CASE("Media File - emits no visitor callbacks when required parsing fails", "[media][unit][visitor]")
  {
    auto const bytes = std::to_array<std::uint8_t>({'b', 'a', 'd'});
    auto const temp = TempFile{bytes, ".flac"};
    auto file = requireValue(File::open(temp.path));
    auto content = RecordedContent{};
    auto visitor = VisitorSpy{content};

    auto result = file.visit(visitor);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
    CHECK(content.callCount() == 0);
  }

  TEST_CASE("Media File - moving transfers stable borrowed views", "[media][unit][lifetime]")
  {
    auto file = requireValue(File::open(audio::test::requireAudioFixture("basic_metadata.flac")));
    auto firstPayload = requireValue(file.audioPayload());
    auto const* const payloadAddress = firstPayload.bytes.data();

    auto movedFile = std::move(file);
    auto secondPayload = requireValue(movedFile.audioPayload());
    auto content = RecordedContent{};
    auto visitor = VisitorSpy{content};

    REQUIRE(movedFile.visit(visitor));
    CHECK(secondPayload.bytes.data() == payloadAddress);
    CHECK(content.callCount() > 0);
  }

  TEST_CASE("Media File - visit emits fields in the documented callback order", "[media][unit][visitor]")
  {
    auto file = requireValue(File::open(audio::test::requireAudioFixture("classical_metadata.mp3")));
    auto content = RecordedContent{};
    auto visitor = VisitorSpy{content};

    REQUIRE(file.visit(visitor));

    using Event = RecordedContent::CallbackEvent;
    using Kind = RecordedContent::CallbackKind;
    auto const expected = std::vector<Event>{
      {Kind::Text, static_cast<std::uint8_t>(TextField::Title)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Artist)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Album)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Composer)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Conductor)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Ensemble)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Genre)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Work)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Movement)},
      {Kind::Text, static_cast<std::uint8_t>(TextField::Soloist)},
      {Kind::Number, static_cast<std::uint8_t>(NumberField::Year)},
      {Kind::Number, static_cast<std::uint8_t>(NumberField::TrackNumber)},
      {Kind::Number, static_cast<std::uint8_t>(NumberField::TrackTotal)},
      {Kind::Number, static_cast<std::uint8_t>(NumberField::MovementNumber)},
      {Kind::Number, static_cast<std::uint8_t>(NumberField::MovementTotal)},
      {Kind::Codec},
      {Kind::Duration},
      {Kind::Bitrate},
      {Kind::SampleRate},
      {Kind::Channels},
      {Kind::BitDepth},
    };

    CHECK(content.events() == expected);
  }

  TEST_CASE("Media File - repeated visits retain earlier decoded string views", "[media][regression][lifetime]")
  {
    auto file = requireValue(File::open(audio::test::requireAudioFixture("basic_metadata.mp3")));
    auto first = RecordedContent{};
    auto firstVisitor = VisitorSpy{first};
    REQUIRE(file.visit(firstVisitor));
    auto const firstTitle = first.text(TextField::Title);
    auto const* const firstAddress = firstTitle.data();

    auto second = RecordedContent{};
    auto secondVisitor = VisitorSpy{second};
    REQUIRE(file.visit(secondVisitor));

    CHECK(firstTitle == "Test Title");
    CHECK(second.text(TextField::Title) == "Test Title");
    CHECK(second.text(TextField::Title).data() == firstAddress);
  }
} // namespace ao::media::file::test
