// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreFoundationString.h"

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreFoundationString - UTF-8 text round trips with explicit length", "[audio][unit][coreaudio]")
  {
    auto const text = std::string{"Aobus \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB5\0UID", 21};
    auto const nativeRes = coreFoundationString(text);
    REQUIRE(nativeRes);

    auto const roundTripRes = utf8String(nativeRes->get());
    REQUIRE(roundTripRes);
    CHECK(*roundTripRes == text);
  }

  TEST_CASE("CoreFoundationString - rejects invalid UTF-8 and null references", "[audio][unit][coreaudio]")
  {
    auto const invalidRes = coreFoundationString(std::string{"\xC3\x28", 2});
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);

    auto const nullRes = utf8String(nullptr);
    REQUIRE_FALSE(nullRes);
    CHECK(nullRes.error().code == Error::Code::InvalidInput);
  }
} // namespace ao::audio::backend::detail::test
