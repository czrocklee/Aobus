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

  TEST_CASE("TagFile - opens parser by file extension", "[tag][unit][factory]")
  {
    SECTION("MP3 extension")
    {
      auto const temp = TempFile{".mp3"};
      auto fileResult = TagFile::open(temp.path);
      REQUIRE(fileResult);
      REQUIRE(*fileResult != nullptr);
      CHECK(dynamic_cast<mpeg::File*>(fileResult->get()) != nullptr);
    }

    SECTION("M4A extension")
    {
      auto const temp = TempFile{".m4a"};
      auto fileResult = TagFile::open(temp.path);
      REQUIRE(fileResult);
      REQUIRE(*fileResult != nullptr);
      CHECK(dynamic_cast<mp4::File*>(fileResult->get()) != nullptr);
    }

    SECTION("FLAC extension")
    {
      auto const temp = TempFile{".flac"};
      auto fileResult = TagFile::open(temp.path);
      REQUIRE(fileResult);
      REQUIRE(*fileResult != nullptr);
      CHECK(dynamic_cast<flac::File*>(fileResult->get()) != nullptr);
    }

    SECTION("Unknown extension")
    {
      auto const temp = TempFile{".txt"};
      auto fileResult = TagFile::open(temp.path);
      REQUIRE_FALSE(fileResult);
      CHECK(fileResult.error().code == Error::Code::NotSupported);
    }

    SECTION("Missing supported file returns an IO error")
    {
      auto fileResult = TagFile::open("/tmp/aobus-missing-file.mp3");
      REQUIRE_FALSE(fileResult);
      CHECK(fileResult.error().code == Error::Code::IoError);
    }
  }
} // namespace ao::tag::test
