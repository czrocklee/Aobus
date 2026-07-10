// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/WasapiStrings.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("WasapiStrings - empty strings remain empty", "[audio][unit][wasapi][strings]")
  {
    CHECK(utf8ToWide({}).empty());
    CHECK(wideToUtf8({}).empty());
  }

  TEST_CASE("WasapiStrings - UTF-8 text round trips through UTF-16", "[audio][unit][wasapi][strings]")
  {
    auto const text = std::string{"Aobus \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB5"};

    CHECK(wideToUtf8(utf8ToWide(text)) == text);
  }

  TEST_CASE("WasapiStrings - explicit lengths preserve embedded nulls", "[audio][unit][wasapi][strings]")
  {
    auto const text = std::string{"left\0right", 10};

    auto const wide = utf8ToWide(text);
    REQUIRE(wide.size() == text.size());
    CHECK(wide[4] == L'\0');
    CHECK(wideToUtf8(wide) == text);
  }
} // namespace ao::audio::backend::detail::test