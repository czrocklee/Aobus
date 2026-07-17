// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/Error.h>
#include <ao/library/LibraryUri.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace ao::library::test
{
  TEST_CASE("LibraryUri - parsing produces canonical root-relative values", "[library][unit][uri]")
  {
    auto const uri = LibraryUri::parse(R"(albums\live\..\song.flac)");

    REQUIRE(uri);
    CHECK(uri->value() == "albums/song.flac");

    for (auto const alias : {std::string_view{"albums"},
                             std::string_view{"albums/"},
                             std::string_view{"albums/."},
                             std::string_view{"albums/live/.."}})
    {
      CAPTURE(alias);
      auto const normalized = LibraryUri::parse(alias);
      REQUIRE(normalized);
      CHECK(normalized->value() == "albums");
    }

    auto const literalPercentEncoding = LibraryUri::parse("literal/%2e%2e/song.flac");
    REQUIRE(literalPercentEncoding);
    CHECK(literalPercentEncoding->value() == "literal/%2e%2e/song.flac");
  }

  TEST_CASE("LibraryUri - parsing owns the storage length limit", "[library][unit][uri]")
  {
    CHECK(LibraryUri::parse(std::string(LibraryUri::kMaxLength, 'a')));

    auto const tooLong = LibraryUri::parse(std::string(LibraryUri::kMaxLength + 1U, 'a'));
    REQUIRE_FALSE(tooLong);
    CHECK(tooLong.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LibraryUri - parsing rejects paths outside the library namespace", "[library][unit][uri]")
  {
    for (auto const input : {std::string_view{},
                             std::string_view{"."},
                             std::string_view{"/song.flac"},
                             std::string_view{"../song.flac"},
                             std::string_view{"folder/../../song.flac"},
                             std::string_view{"C:/music/song.flac"},
                             std::string_view{"C:music/song.flac"},
                             std::string_view{"//server/share/song.flac"}})
    {
      CAPTURE(input);
      auto const result = LibraryUri::parse(input);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("LibraryUri - parsing rejects control characters", "[library][unit][uri]")
  {
    for (auto const input : {std::string_view{"line\nbreak.flac"},
                             std::string_view{"tab\tname.flac"},
                             std::string_view{"delete\x7f.flac"}})
    {
      CAPTURE(input);
      auto const result = LibraryUri::parse(input);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("control characters"));
    }
  }

  TEST_CASE("LibraryUri - resolution accepts missing roots and ordinary missing suffixes", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const missingRoot = temp.path() / "future-music";
    auto const futureUri = LibraryUri::parse("future/song.flac");
    REQUIRE(futureUri);
    auto const future = futureUri->resolveUnder(missingRoot);
    REQUIRE(future);
    CHECK(*future == missingRoot / "future" / "song.flac");

    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    auto const directUri = LibraryUri::parse("upcoming/song.flac");
    REQUIRE(directUri);
    auto const direct = directUri->resolveUnder(root);
    REQUIRE(direct);
    CHECK(*direct == root / "upcoming" / "song.flac");
  }

  TEST_CASE("LibraryUri - resolution accepts in-root symlinks", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const album = root / "album";
    std::filesystem::create_directories(album);
    auto const symlink = ao::test::SymlinkFixture{album, root / "alias", ao::test::SymlinkType::Directory};

    auto const aliasUri = LibraryUri::parse("alias/song.flac");
    REQUIRE(aliasUri);
    auto const alias = aliasUri->resolveUnder(root);
    REQUIRE(alias);
    CHECK(*alias == album / "song.flac");
  }

  TEST_CASE("LibraryUri - resolution rejects a symlink escaping the root", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const outside = temp.path() / "outside";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    auto const symlink = ao::test::SymlinkFixture{outside, root / "alias", ao::test::SymlinkType::Directory};
    auto const uri = LibraryUri::parse("alias/song.flac");

    REQUIRE(uri);
    auto const resolved = uri->resolveUnder(root);
    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("LibraryUri - resolution rejects a dangling symlink", "[library][regression][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const outside = temp.path() / "outside";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    auto const symlink =
      ao::test::SymlinkFixture{outside / "missing", root / "alias", ao::test::SymlinkType::Directory};
    auto const uri = LibraryUri::parse("alias/song.flac");

    REQUIRE(uri);
    auto const resolved = uri->resolveUnder(root);
    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().code == Error::Code::InvalidInput);
    CHECK(resolved.error().message.contains("unresolved symlink"));
  }

  TEST_CASE("LibraryUri - resolution requires an item below the root", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    auto const symlink = ao::test::SymlinkFixture{root, root / "self", ao::test::SymlinkType::Directory};
    auto const uri = LibraryUri::parse("self");

    REQUIRE(uri);
    auto const resolved = uri->resolveUnder(root);
    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().code == Error::Code::InvalidInput);
  }
} // namespace ao::library::test
