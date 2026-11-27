// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/MappedFile.h>

#include "test/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <fstream>
#include <ios>
#include <utility>

namespace ao::utility::test
{
  TEST_CASE("MappedFile - mappings can be closed, replaced, or transferred")
  {
    auto const temp = ao::test::TempDir{};
    auto const first = temp.path() / "first.bin";
    auto const second = temp.path() / "second.bin";
    REQUIRE((std::ofstream{first, std::ios::binary} << "first").good());
    REQUIRE((std::ofstream{second, std::ios::binary} << "second").good());

    auto source = MappedFile{};
    CHECK_FALSE(source.isMapped());
    CHECK(source.bytes().empty());
    REQUIRE(source.map(first));
    CHECK(source.isMapped());
    CHECK(bytes::stringView(source.bytes()) == "first");

    auto const reuseSource = [&]
    {
      REQUIRE_FALSE(source.isMapped());
      CHECK(source.bytes().empty());
      REQUIRE(source.map(second));
      CHECK(bytes::stringView(source.bytes()) == "second");
    };

    SECTION("explicit close")
    {
      source.unmap();
      reuseSource();
    }

    SECTION("failed remap discards the old mapping")
    {
      auto const res = source.map(temp.path() / "missing.bin");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
      reuseSource();
    }

    SECTION("an empty file cannot provide a mapped region")
    {
      auto const empty = temp.path() / "empty.bin";
      REQUIRE(std::ofstream{empty, std::ios::binary}.good());
      auto const res = source.map(empty);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
      reuseSource();
    }

    SECTION("transfer to a new or previously mapped file")
    {
      auto const borrow = source.bytes();
      auto const replace = GENERATE(false, true);
      CAPTURE(replace);
      auto target = [&]
      {
        if (!replace)
        {
          return MappedFile{std::move(source)};
        }

        auto previous = MappedFile{};
        REQUIRE(previous.map(second));
        previous = std::move(source);
        return previous;
      }();
      reuseSource();
      REQUIRE(target.isMapped());
      REQUIRE(target.bytes().data() == borrow.data());
      CHECK(bytes::stringView(target.bytes()) == "first");
      CHECK(bytes::stringView(borrow) == "first");
    }
  }
} // namespace ao::utility::test
