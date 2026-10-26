// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/media/file/detail/Content.h"
#include <ao/media/file/Visitor.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <deque>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::detail::test
{
  TEST_CASE("ContentBuilder - CUE sheets keep distinct nonempty source values in order")
  {
    auto builder = ContentBuilder::makeEmpty();
    builder.cueSheet("");
    builder.cueSheet(builder.own("first"));
    builder.cueSheet(builder.own("second"));
    // Equal bytes in separate storage are still the same sheet.
    builder.cueSheet(builder.own("first"));
    builder.cueSheet("FIRST");
    builder.cueSheet(" first");
    builder.cueSheet(" ");
    builder.cueSheet("");

    auto const content = std::move(builder).finish();

    CHECK(content.cueSheets == std::vector<std::string_view>{"first", "second", "FIRST", " first", " "});
  }

  TEST_CASE("ContentBuilder - many distinct CUE sheets retain source order across repeats")
  {
    constexpr std::size_t kSheetCount = 80000;
    auto expected = std::vector<std::string_view>{};
    auto content = Content{};

    {
      auto builder = ContentBuilder::makeEmpty();

      for (std::size_t index = 0; index < kSheetCount; ++index)
      {
        auto const sheet = builder.own(std::format("sheet {}", index));
        expected.push_back(sheet);
        builder.cueSheet(sheet);
      }

      for (std::size_t index = kSheetCount; index > 0; --index)
      {
        builder.cueSheet(builder.own(std::string{expected[index - 1]}));
      }

      content = std::move(builder).finish();
    }

    // Both the ordered sheets and their owned bytes survive builder teardown.
    CHECK(content.cueSheets == expected);
  }

  TEST_CASE("ContentBuilder - moving preserves CUE membership while building continues")
  {
    auto content = Content{};

    SECTION("move construction transfers membership")
    {
      auto source = ContentBuilder::makeEmpty();
      source.cueSheet(source.own("first"));
      source.cueSheet(source.own("second"));
      auto moved = ContentBuilder{std::move(source)};
      moved.cueSheet(moved.own("first"));
      moved.cueSheet(moved.own("third"));
      content = std::move(moved).finish();
    }

    SECTION("move assignment replaces earlier membership")
    {
      auto source = ContentBuilder::makeEmpty();
      source.cueSheet(source.own("first"));
      source.cueSheet(source.own("second"));
      auto moved = ContentBuilder::makeEmpty();
      moved.cueSheet(moved.own("third"));
      moved = std::move(source);
      moved.cueSheet(moved.own("first"));
      moved.cueSheet(moved.own("third"));
      content = std::move(moved).finish();
    }

    CHECK(content.cueSheets == std::vector<std::string_view>{"first", "second", "third"});
  }

  TEST_CASE("MetadataBuilder - repeated multi-name descriptors join in source order")
  {
    auto builder = ContentBuilder::makeEmpty();
    auto& metadata = builder.metadata();

    metadata.artist("Artist A").artist("Artist B").artist("Artist C");
    metadata.albumArtist("Album Artist A").albumArtist("Album Artist B");
    metadata.composer("Composer A").composer("Composer B");

    CHECK(metadata.artist() == "Artist A; Artist B; Artist C");
    CHECK(metadata.albumArtist() == "Album Artist A; Album Artist B");
    CHECK(metadata.composer() == "Composer A; Composer B");
  }

  TEST_CASE("MetadataBuilder - a joined value owns its text")
  {
    auto builder = ContentBuilder::makeEmpty();
    auto first = std::string{"Artist A"};
    auto second = std::string{"Artist B"};

    // Both values are borrowed views; the joined value must not alias them.
    builder.metadata().artist(first).artist(second);
    first.assign(first.size(), 'x');
    second.assign(second.size(), 'x');

    auto const content = std::move(builder).finish();
    CHECK(content.texts[static_cast<std::size_t>(TextField::Artist)] == "Artist A; Artist B");
  }

  TEST_CASE("MetadataBuilder - blank and already joined values add nothing")
  {
    auto builder = ContentBuilder::makeEmpty();
    auto& metadata = builder.metadata();

    SECTION("a blank value neither sets nor joins")
    {
      metadata.artist(" ").artist("Artist A").artist("").artist("\t");
      CHECK(metadata.artist() == "Artist A");
    }

    SECTION("an alias carrying the same name joins it once")
    {
      metadata.albumArtist("Album Artist").albumArtist("Album Artist");
      CHECK(metadata.albumArtist() == "Album Artist");
    }

    SECTION("a repeat of an earlier joined name is not joined again")
    {
      metadata.composer("Composer A").composer("Composer B").composer("Composer A");
      CHECK(metadata.composer() == "Composer A; Composer B");
    }

    SECTION("a name that differs only in case is a different name")
    {
      metadata.artist("Artist").artist("ARTIST");
      CHECK(metadata.artist() == "Artist; ARTIST");
    }

    SECTION("a value that already holds the separator stays verbatim")
    {
      metadata.artist("Artist A; Artist B").artist("Artist C");
      CHECK(metadata.artist() == "Artist A; Artist B; Artist C");
    }

    SECTION("a repeat of a value that holds the separator is joined once")
    {
      metadata.artist("Artist A").artist("Artist B; Artist C").artist("Artist B; Artist C");
      CHECK(metadata.artist() == "Artist A; Artist B; Artist C");
    }

    SECTION("a value equal to a name inside an earlier value still joins")
    {
      metadata.artist("Artist A; Artist B").artist("Artist A");
      CHECK(metadata.artist() == "Artist A; Artist B; Artist A");
    }
  }

  TEST_CASE("MetadataBuilder - joining many values retains text linear in their size")
  {
    constexpr std::size_t kValueCount = 5000;
    // The builder keeps views of its values, so they outlive the content.
    auto values = std::deque<std::string>{};
    auto builder = ContentBuilder::makeEmpty();
    std::size_t expectedSize = 0;

    for (std::size_t index = 0; index < kValueCount; ++index)
    {
      auto const& value = values.emplace_back(std::format("Artist {}", index));
      expectedSize += value.size() + (index == 0 ? 0 : 2);
      builder.metadata().artist(value);
    }

    auto const content = std::move(builder).finish();
    auto const joined = content.texts[static_cast<std::size_t>(TextField::Artist)];
    std::size_t retainedBytes = 0;

    for (auto const& owned : content.ownedStrings)
    {
      retainedBytes += owned.size();
    }

    CHECK(joined.size() == expectedSize);
    CHECK(joined.starts_with("Artist 0; Artist 1; "));
    CHECK(joined.ends_with("; Artist 4999"));
    // Copying the joined text at every value would keep each prefix, about
    // kValueCount / 2 times the final size.
    CHECK(retainedBytes <= 2 * joined.size());
  }

  TEST_CASE("MetadataBuilder - Genre keeps its first nonblank value")
  {
    auto builder = ContentBuilder::makeEmpty();
    auto& metadata = builder.metadata();

    metadata.genre(" ").genre("Rock").genre("Pop");

    CHECK(metadata.genre() == "Rock");
  }

  TEST_CASE("MetadataBuilder - replace supersedes a folded value")
  {
    auto builder = ContentBuilder::makeEmpty();
    auto& metadata = builder.metadata();

    metadata.artist("Artist A").artist("Artist B").genre("Rock");
    metadata.replace(TextField::Artist, "Later Artist").replace(TextField::Genre, "Later Genre");

    CHECK(metadata.artist() == "Later Artist");
    CHECK(metadata.genre() == "Later Genre");

    // A later join starts from the replaced value alone.
    metadata.artist("Later Artist").artist("Artist A");
    CHECK(metadata.artist() == "Later Artist; Artist A");
  }

  TEST_CASE("MetadataBuilder - single-value text fields keep the last value")
  {
    auto builder = ContentBuilder::makeEmpty();
    auto& metadata = builder.metadata();

    metadata.title("First").title("Second").album("First").album("Second");

    CHECK(metadata.title() == "Second");
    CHECK(metadata.album() == "Second");
  }
} // namespace ao::media::file::detail::test
