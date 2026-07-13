// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TestAtoms.h"
#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::media::mp4::test
{
  namespace
  {
    std::vector<std::byte> toBytes(std::vector<std::uint8_t> const& bytes)
    {
      auto result = std::vector<std::byte>{};
      result.reserve(bytes.size());

      for (auto const byte : bytes)
      {
        result.push_back(static_cast<std::byte>(byte));
      }

      return result;
    }
  } // namespace

  TEST_CASE("MP4 AtomCursor - visits immediate children in file order", "[media][unit][mp4]")
  {
    auto data = std::vector<std::uint8_t>{};
    ao::test::mp4::addAtom(data, "ftyp", {1, 2});
    ao::test::mp4::addAtom(data, "free", {3});
    auto const bytes = toBytes(data);
    auto cursor = fromBuffer(bytes).children();

    auto firstResult = cursor.next();
    REQUIRE(firstResult);
    REQUIRE(*firstResult);
    CHECK((*firstResult)->type() == "ftyp");
    CHECK((*firstResult)->bytes().size() == 10);

    auto secondResult = cursor.next();
    REQUIRE(secondResult);
    REQUIRE(*secondResult);
    CHECK((*secondResult)->type() == "free");
    CHECK((*secondResult)->bytes().size() == 9);

    auto endResult = cursor.next();
    REQUIRE(endResult);
    CHECK_FALSE(*endResult);
  }

  TEST_CASE("MP4 AtomCursor - accepts extended and end-of-file atom sizes", "[media][regression][mp4]")
  {
    auto data = ao::test::mp4::makeExtendedAtom("free", {1, 2});
    auto const endOfFileAtom = ao::test::mp4::makeEndOfFileAtom("mdat", {3, 4, 5});
    data.insert(data.end(), endOfFileAtom.begin(), endOfFileAtom.end());
    auto const bytes = toBytes(data);
    auto cursor = fromBuffer(bytes).children();

    auto const extendedResult = cursor.next();
    REQUIRE(extendedResult);
    REQUIRE(*extendedResult);
    CHECK((*extendedResult)->type() == "free");
    CHECK((*extendedResult)->bytes().size() == 18);
    CHECK((*extendedResult)->payload().size() == 2);

    auto const endOfFileResult = cursor.next();
    REQUIRE(endOfFileResult);
    REQUIRE(*endOfFileResult);
    CHECK((*endOfFileResult)->type() == "mdat");
    CHECK((*endOfFileResult)->bytes().size() == 11);
    CHECK((*endOfFileResult)->payload().size() == 3);

    auto const endResult = cursor.next();
    REQUIRE(endResult);
    CHECK_FALSE(*endResult);
  }

  TEST_CASE("MP4 AtomCursor - rejects malformed extended atom sizes", "[media][unit][mp4][error]")
  {
    SECTION("Truncated extended header")
    {
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(data, 1);
      data.insert(data.end(), {'m', 'd', 'a', 't'});
      auto const bytes = toBytes(data);
      auto cursor = fromBuffer(bytes).children();

      auto const result = cursor.next();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Extended size smaller than its header")
    {
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(data, 1);
      data.insert(data.end(), {'m', 'd', 'a', 't'});
      ao::test::mp4::appendBe64(data, 15);
      auto const bytes = toBytes(data);
      auto cursor = fromBuffer(bytes).children();

      auto const result = cursor.next();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Extended size exceeds the remaining container")
    {
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(data, 1);
      data.insert(data.end(), {'m', 'd', 'a', 't'});
      ao::test::mp4::appendBe64(data, 17);
      auto const bytes = toBytes(data);
      auto cursor = fromBuffer(bytes).children();

      auto const result = cursor.next();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("MP4 AtomCursor - rejects end-of-file size inside a container", "[media][unit][mp4][error]")
  {
    auto const child = ao::test::mp4::makeEndOfFileAtom("free", {1});
    auto const bytes = toBytes(ao::test::mp4::makeAtom("moov", child));
    auto rootCursor = fromBuffer(bytes).children();
    auto const movieResult = rootCursor.next();
    REQUIRE(movieResult);
    REQUIRE(*movieResult);

    auto childCursor = (*movieResult)->children();
    auto const childResult = childCursor.next();

    REQUIRE_FALSE(childResult);
    CHECK(childResult.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("MP4 AtomView - short typed layout returns no view", "[media][regression][mp4]")
  {
    auto const bytes = toBytes(ao::test::mp4::makeAtom("stsd", {}));
    auto cursor = fromBuffer(bytes).children();
    auto atomResult = cursor.next();

    REQUIRE(atomResult);
    REQUIRE(*atomResult);
    CHECK((*atomResult)->tryLayout<StsdAtomLayout>() == nullptr);
  }

  TEST_CASE("MP4 findAtom - returns a non-owning nested path view", "[media][unit][mp4]")
  {
    auto const stsd = ao::test::mp4::makeStsdAtom("alac");
    auto const stbl = ao::test::mp4::makeAtom("stbl", stsd);
    auto const track = ao::test::mp4::makeTrackAtom("soun", stbl);
    auto const bytes = toBytes(ao::test::mp4::makeAtom("moov", track));
    auto const path = std::to_array<std::string_view>({"root", "moov", "trak", "mdia", "minf", "stbl", "stsd"});

    auto result = findAtom(fromBuffer(bytes), path);

    REQUIRE(result);
    REQUIRE(*result);
    CHECK((*result)->type() == "stsd");
    CHECK((*result)->bytes().data() >= bytes.data());
    CHECK((*result)->bytes().data() + (*result)->bytes().size() <= bytes.data() + bytes.size());
  }

  TEST_CASE("MP4 AtomCursor - reports malformed trailing child boundaries", "[media][unit][mp4]")
  {
    auto data = ao::test::mp4::makeAtom("free", {1});
    ao::test::mp4::appendBe32(data, 100);
    data.insert(data.end(), {'b', 'a', 'd', '!'});
    auto const bytes = toBytes(data);
    auto cursor = fromBuffer(bytes).children();

    auto firstResult = cursor.next();
    REQUIRE(firstResult);
    REQUIRE(*firstResult);
    CHECK((*firstResult)->type() == "free");

    auto malformedResult = cursor.next();
    REQUIRE_FALSE(malformedResult);
    CHECK(malformedResult.error().code == Error::Code::CorruptData);
    CHECK(malformedResult.error().message == "mp4 atom size exceeds its container boundary");
  }

  TEST_CASE("MP4 findAtom - stops before unrelated siblings after the matched path", "[media][regression][mp4]")
  {
    auto data = ao::test::mp4::makeAtom("moov", {});
    ao::test::mp4::appendBe32(data, 100);
    data.insert(data.end(), {'b', 'a', 'd', '!'});
    auto const bytes = toBytes(data);
    auto const path = std::to_array<std::string_view>({"root", "moov"});

    auto result = findAtom(fromBuffer(bytes), path);

    REQUIRE(result);
    REQUIRE(*result);
    CHECK((*result)->type() == "moov");
  }
} // namespace ao::media::mp4::test
