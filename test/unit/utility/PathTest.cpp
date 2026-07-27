// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::utility::test
{
  TEST_CASE("Path - UTF-8 text round trips through native paths", "[utility][regression][path]")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82/"
                                      "Dvo\xC5\x99\xC3\xA1k.flac"};

    auto const path = pathFromUtf8(expected);

    CHECK(pathToGenericUtf8(path) == expected);
    CHECK(pathToUtf8(path.filename()) == "Dvo\xC5\x99\xC3\xA1k.flac");
  }
} // namespace ao::utility::test
