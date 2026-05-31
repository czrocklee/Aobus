// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/flac/File.h"
#include "lib/tag/mp4/File.h"
#include "lib/tag/mpeg/File.h"
#include "test/unit/TestUtils.h"
#include <ao/tag/TagFile.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace ao::tag::test
{
  using namespace ao::test;

  TEST_CASE("TagFile - Factory", "[tag][unit][factory]")
  {
    SECTION("MP3 extension")
    {
      auto const temp = TempFile{".mp3"};
      auto filePtr = TagFile::open(temp.path);
      REQUIRE(filePtr != nullptr);
      CHECK(dynamic_cast<mpeg::File*>(filePtr.get()) != nullptr);
    }

    SECTION("M4A extension")
    {
      auto const temp = TempFile{".m4a"};
      auto filePtr = TagFile::open(temp.path);
      REQUIRE(filePtr != nullptr);
      CHECK(dynamic_cast<mp4::File*>(filePtr.get()) != nullptr);
    }

    SECTION("FLAC extension")
    {
      auto const temp = TempFile{".flac"};
      auto filePtr = TagFile::open(temp.path);
      REQUIRE(filePtr != nullptr);
      CHECK(dynamic_cast<flac::File*>(filePtr.get()) != nullptr);
    }

    SECTION("Unknown extension")
    {
      auto const temp = TempFile{".txt"};
      auto filePtr = TagFile::open(temp.path);
      CHECK(filePtr == nullptr);
    }
  }
} // namespace ao::tag::test
