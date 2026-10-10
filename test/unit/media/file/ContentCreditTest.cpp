// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/media/file/detail/Content.h"
#include "test/unit/media/file/TestFile.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::media::file::detail::test
{
  namespace
  {
    class BorrowedCreditSpy final : public Visitor
    {
    public:
      explicit BorrowedCreditSpy(Content const& content)
        : _content{content}
      {
      }
      void text([[maybe_unused]] TextField field, [[maybe_unused]] std::string_view value) override {}
      void number([[maybe_unused]] NumberField field, [[maybe_unused]] std::uint16_t value) override {}
      void codec([[maybe_unused]] AudioCodec value) override {}
      void duration([[maybe_unused]] std::chrono::milliseconds duration) override {}
      void bitrate([[maybe_unused]] Bitrate value) override {}
      void sampleRate([[maybe_unused]] SampleRate value) override {}
      void channels([[maybe_unused]] Channels value) override {}
      void bitDepth([[maybe_unused]] BitDepth value) override {}
      void picture([[maybe_unused]] PictureType type, [[maybe_unused]] std::span<std::byte const> bytes) override {}

      void visitCredits(std::span<CreditView const> entries) override
      {
        ++_callCount;
        REQUIRE_FALSE(entries.empty());
        REQUIRE(entries.size() == _content.credits.size());
        CHECK(entries.data() == _content.credits.data());

        for (std::size_t index = 0; index < entries.size(); ++index)
        {
          CHECK(entries[index].name.data() == _content.credits[index].name.data());
          CHECK(entries[index].name == _content.credits[index].name);
          CHECK(entries[index].kind == _content.credits[index].kind);
          CHECK(entries[index].role.data() == _content.credits[index].role.data());
          CHECK(entries[index].role == _content.credits[index].role);
        }
      }

      std::size_t callCount() const { return _callCount; }

    private:
      Content const& _content;
      std::size_t _callCount = 0;
    };
  } // namespace

  TEST_CASE("Content credits - retain all initial kinds and repeated entries in source order", "[media][unit][content]")
  {
    auto builder = ContentBuilder::makeEmpty();
    builder.metadata().soloist("Featured soloist").conductor("Conductor").ensemble("Ensemble");
    builder.metadata()
      .credit("Ada", CreditKind::Performer, "piano")
      .credit("Bob", CreditKind::Performer)
      .credit("Ada", CreditKind::Performer, "piano")
      .credit("Ada", CreditKind::Performer, "organ")
      .conductor("Conductor")
      .ensemble(" \t ")
      .soloist("");
    auto const content = std::move(builder).finish();

    REQUIRE(content.credits.size() == 8);
    CHECK(content.credits[0].name == "Featured soloist");
    CHECK(content.credits[0].kind == CreditKind::Soloist);
    CHECK(content.credits[1].name == "Conductor");
    CHECK(content.credits[1].kind == CreditKind::Conductor);
    CHECK(content.credits[2].name == "Ensemble");
    CHECK(content.credits[2].kind == CreditKind::Ensemble);
    CHECK(content.credits[3].name == "Ada");
    CHECK(content.credits[3].role == "piano");
    CHECK(content.credits[4].name == "Bob");
    CHECK(content.credits[4].role.empty());
    CHECK(content.credits[5].name == "Ada");
    CHECK(content.credits[5].role == "piano");
    CHECK(content.credits[6].name == "Ada");
    CHECK(content.credits[6].role == "organ");
    CHECK(content.credits[7].name == "Conductor");
    CHECK(content.credits[7].kind == CreditKind::Conductor);

    for (auto const& credit : std::span{content.credits}.subspan(3, 4))
    {
      CHECK(credit.kind == CreditKind::Performer);
    }
  }

  TEST_CASE("Content credits - copy source text and keep it stable through arena growth and builder moves",
            "[media][unit][content]")
  {
    auto content = []
    {
      auto builder = ContentBuilder::makeEmpty();
      auto name = std::string{"Ada"};
      auto role = std::string{"piano"};
      builder.metadata().credit(name, CreditKind::Performer, role);
      name.assign("Changed");
      role.assign("Changed");

      // Short strings must survive moves and later arena additions too.
      for (std::size_t index = 0; index < 256; ++index)
      {
        std::ignore = builder.own(std::string(64, 'x'));
      }

      auto moved = ContentBuilder{std::move(builder)};
      auto replacement = ContentBuilder::makeEmpty();
      replacement.metadata().credit("Discarded", CreditKind::Soloist, "Discarded role");
      replacement = std::move(moved);
      replacement.metadata().credit(replacement.own("Bob"), CreditKind::Performer);
      return std::move(replacement).finish();
    }();

    REQUIRE(content.credits.size() == 2);
    CHECK(content.credits[0].name == "Ada");
    CHECK(content.credits[0].kind == CreditKind::Performer);
    CHECK(content.credits[0].role == "piano");
    CHECK(content.credits[1].name == "Bob");
    CHECK(content.credits[1].kind == CreditKind::Performer);
    CHECK(content.credits[1].role.empty());

    for (auto const& credit : content.credits)
    {
      CHECK(
        std::ranges::any_of(content.ownedStrings, [&](auto const& text) { return text.data() == credit.name.data(); }));
    }
  }

  TEST_CASE("Content credits - callback borrows the content span and text and completes before visit returns",
            "[media][unit][content]")
  {
    auto builder = ContentBuilder::makeEmpty();
    builder.metadata().credit("Ada", CreditKind::Performer, "piano").soloist("Anne");
    auto const content = std::move(builder).finish();
    auto visitor = BorrowedCreditSpy{content};
    CHECK(visitor.callCount() == 0);
    content.visit(visitor);
    CHECK(visitor.callCount() == 1);
  }

  TEST_CASE("Content credits - visitor receives one borrowed list synchronously after scalar and cover callbacks",
            "[media][unit][content]")
  {
    using RecordedContent = ao::media::file::test::RecordedContent;
    using Event = RecordedContent::CallbackEvent;
    using Kind = RecordedContent::CallbackKind;
    using Credit = RecordedContent::Credit;

    auto const pictureBytes = std::array{std::byte{0x11}, std::byte{0x22}};
    auto const scalarAndCover = std::vector<Event>{
      {Kind::Text, static_cast<std::uint8_t>(TextField::Title)},
      {Kind::Number, static_cast<std::uint8_t>(NumberField::Year)},
      {Kind::Picture, static_cast<std::uint8_t>(PictureType::FrontCover)},
    };

    SECTION("nonempty list is emitted once before visit returns and the copy outlives its owner")
    {
      auto const owned = [&]
      {
        auto const name = std::string{"Ada"};
        auto const piano = std::string{"piano"};
        auto const bob = std::string{"Bob"};
        auto const organ = std::string{"organ"};
        auto builder = ContentBuilder::makeEmpty();
        builder.metadata().title("Title").soloist("Featured").year(1981);
        builder.coverArt().add(PictureType::FrontCover, pictureBytes);
        builder.metadata()
          .credit(name, CreditKind::Performer, piano)
          .credit(name, CreditKind::Performer, piano)
          .credit(bob, CreditKind::Performer)
          .credit(name, CreditKind::Performer, organ);
        auto const content = std::move(builder).finish();
        auto recorded = RecordedContent{};
        auto visitor = ao::media::file::test::VisitorSpy{recorded};
        content.visit(visitor);

        auto expectedEvents = scalarAndCover;
        expectedEvents.push_back({Kind::Credits});
        CHECK(recorded.events() == expectedEvents);
        REQUIRE(recorded.pictures().size() == 1);
        CHECK(recorded.pictures().front().type == PictureType::FrontCover);
        CHECK(recorded.pictures().front().bytes.size() == pictureBytes.size());
        REQUIRE(recorded.credits().size() == content.credits.size());

        for (std::size_t index = 0; index < content.credits.size(); ++index)
        {
          CHECK(recorded.credits()[index].name.data() != content.credits[index].name.data());

          if (!content.credits[index].role.empty())
          {
            CHECK(recorded.credits()[index].role.data() != content.credits[index].role.data());
          }
        }

        return recorded.credits();
      }();

      CHECK(owned == std::vector<Credit>{
                       {.name = "Featured", .kind = CreditKind::Soloist},
                       {.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
                       {.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
                       {.name = "Bob", .kind = CreditKind::Performer},
                       {.name = "Ada", .kind = CreditKind::Performer, .role = "organ"},
                     });
    }

    SECTION("empty or blank-only list emits no callback")
    {
      auto builder = ContentBuilder::makeEmpty();
      builder.metadata().title("Title").year(1981).soloist(" \t\n\r\f\v").orchestra("");
      builder.coverArt().add(PictureType::FrontCover, pictureBytes);
      auto const content = std::move(builder).finish();
      auto recorded = RecordedContent{};
      auto visitor = ao::media::file::test::VisitorSpy{recorded};
      content.visit(visitor);

      CHECK(recorded.events() == scalarAndCover);
      CHECK(recorded.credits().empty());
    }
  }
} // namespace ao::media::file::detail::test
