// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/LibraryUri.h>

#include "lib/library/LibraryUriValidation.h"
#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("LibraryUri - parsing produces canonical root-relative values", "[library][unit][uri]")
  {
    auto const uriRes = LibraryUri::parse(R"(albums\live\..\song.flac)");

    REQUIRE(uriRes);
    CHECK(uriRes->value() == "albums/song.flac");

    for (auto const alias : {std::string_view{"albums"},
                             std::string_view{"albums/"},
                             std::string_view{"albums/."},
                             std::string_view{"albums/live/.."}})
    {
      CAPTURE(alias);
      auto const normalizedRes = LibraryUri::parse(alias);
      REQUIRE(normalizedRes);
      CHECK(normalizedRes->value() == "albums");
    }

    auto const literalPercentEncodingRes = LibraryUri::parse("literal/%2e%2e/song.flac");
    REQUIRE(literalPercentEncodingRes);
    CHECK(literalPercentEncodingRes->value() == "literal/%2e%2e/song.flac");
  }

  TEST_CASE("LibraryUri - persisted canonical check agrees with parse normalization", "[library][unit][uri]")
  {
    STATIC_REQUIRE(noexcept(detail::isCanonicalLibraryUri(std::string_view{})));

    for (auto const text : {std::string_view{"song.flac"},
                            std::string_view{"artist/album/song.flac"},
                            std::string_view{"literal/%2e%2e/song.flac"},
                            std::string_view{"Dvo\xC5\x99\xC3\xA1k/song.flac"}})
    {
      CAPTURE(text);
      auto const parsedRes = LibraryUri::parse(text);
      REQUIRE(parsedRes);
      CHECK(parsedRes->value() == text);
      CHECK(detail::isCanonicalLibraryUri(text));
    }

    auto const maximum = std::string(LibraryUri::kMaxLength, 'a');
    REQUIRE(LibraryUri::parse(maximum));
    CHECK(detail::isCanonicalLibraryUri(maximum));

    auto nonCanonical = std::vector<std::string>{"",
                                                 ".",
                                                 "album/",
                                                 "./song.flac",
                                                 "album//song.flac",
                                                 "album/./song.flac",
                                                 "album/live/../song.flac",
                                                 R"(album\song.flac)",
                                                 "/song.flac",
                                                 "C:music/song.flac",
                                                 "line\nbreak.flac",
                                                 std::string(LibraryUri::kMaxLength + 1U, 'a')};
    nonCanonical.emplace_back("nul\0byte.flac", 13U);

    for (auto const& text : nonCanonical)
    {
      CAPTURE(text);
      auto const parsedRes = LibraryUri::parse(text);
      CHECK_FALSE((parsedRes && parsedRes->value() == text));
      CHECK_FALSE(detail::isCanonicalLibraryUri(text));
    }
  }

  TEST_CASE("LibraryUri - parsing owns the storage length limit", "[library][unit][uri]")
  {
    CHECK(LibraryUri::parse(std::string(LibraryUri::kMaxLength, 'a')));

    auto const tooLongRes = LibraryUri::parse(std::string(LibraryUri::kMaxLength + 1U, 'a'));
    REQUIRE_FALSE(tooLongRes);
    CHECK(tooLongRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LibraryUri - UTF-8 names resolve through native filesystem paths", "[library][regression][uri]")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82/"
                                      "Dvo\xC5\x99\xC3\xA1k.flac"};
    auto const uriRes = LibraryUri::parse(expected);
    REQUIRE(uriRes);
    CHECK(uriRes->value() == expected);

    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);

    auto const resolvedRes = uriRes->resolveUnder(root);

    REQUIRE(resolvedRes);
    CHECK(utility::pathToGenericUtf8(resolvedRes->lexically_relative(root)) == expected);
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
    auto const futureUriRes = LibraryUri::parse("future/song.flac");
    REQUIRE(futureUriRes);
    auto const futureRes = futureUriRes->resolveUnder(missingRoot);
    REQUIRE(futureRes);
    CHECK(*futureRes == missingRoot / "future" / "song.flac");

    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    auto const directUriRes = LibraryUri::parse("upcoming/song.flac");
    REQUIRE(directUriRes);
    auto const directRes = directUriRes->resolveUnder(root);
    REQUIRE(directRes);
    CHECK(*directRes == root / "upcoming" / "song.flac");
  }

  TEST_CASE("LibraryUri - resolution accepts in-root symlinks", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const album = root / "album";
    std::filesystem::create_directories(album);
    auto const symlink = ao::test::SymlinkFixture{album, root / "alias", ao::test::SymlinkType::Directory};

    auto const aliasUriRes = LibraryUri::parse("alias/song.flac");
    REQUIRE(aliasUriRes);
    auto const aliasRes = aliasUriRes->resolveUnder(root);
    REQUIRE(aliasRes);
    CHECK(*aliasRes == album / "song.flac");
  }

  TEST_CASE("LibraryUri - resolution rejects a symlink escaping the root", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const outside = temp.path() / "outside";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    auto const symlink = ao::test::SymlinkFixture{outside, root / "alias", ao::test::SymlinkType::Directory};
    auto const uriRes = LibraryUri::parse("alias/song.flac");

    REQUIRE(uriRes);
    auto const resolvedRes = uriRes->resolveUnder(root);
    REQUIRE_FALSE(resolvedRes);
    CHECK(resolvedRes.error().code == Error::Code::InvalidInput);
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
    auto const uriRes = LibraryUri::parse("alias/song.flac");

    REQUIRE(uriRes);
    auto const resolvedRes = uriRes->resolveUnder(root);
    REQUIRE_FALSE(resolvedRes);
    CHECK(resolvedRes.error().code == Error::Code::InvalidInput);
    CHECK(resolvedRes.error().message.contains("unresolved symlink"));
  }

  TEST_CASE("LibraryUri - resolution requires an item below the root", "[library][unit][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    auto const symlink = ao::test::SymlinkFixture{root, root / "self", ao::test::SymlinkType::Directory};
    auto const uriRes = LibraryUri::parse("self");

    REQUIRE(uriRes);
    auto const resolvedRes = uriRes->resolveUnder(root);
    REQUIRE_FALSE(resolvedRes);
    CHECK(resolvedRes.error().code == Error::Code::InvalidInput);
  }
} // namespace ao::library::test
