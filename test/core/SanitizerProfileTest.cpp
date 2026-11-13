// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

namespace ao::test
{
  namespace
  {
    // test/BUILD.bazel names the selected sanitizer profile. A rule-based
    // toolchain silently ignores a requested feature that it does not know, so
    // the compiler's own instrumentation facts must agree with that selection.
#ifdef AOBUS_EXPECT_ADDRESS_SANITIZER
    constexpr bool kAddressSanitizerExpected = true;
#else
    constexpr bool kAddressSanitizerExpected = false;
#endif

#ifdef AOBUS_EXPECT_UNDEFINED_BEHAVIOR_SANITIZER
    constexpr bool kUndefinedBehaviorSanitizerExpected = true;
#else
    constexpr bool kUndefinedBehaviorSanitizerExpected = false;
#endif

#ifdef AOBUS_EXPECT_THREAD_SANITIZER
    constexpr bool kThreadSanitizerExpected = true;
#else
    constexpr bool kThreadSanitizerExpected = false;
#endif

#ifdef __has_feature
    // Clang on Linux and macOS reports each enabled sanitizer as a feature.
    constexpr bool kAddressSanitizerActive = __has_feature(address_sanitizer) != 0;
    constexpr bool kUndefinedBehaviorSanitizerActive = __has_feature(undefined_behavior_sanitizer) != 0;
    constexpr bool kThreadSanitizerActive = __has_feature(thread_sanitizer) != 0;
#elifdef __SANITIZE_ADDRESS__
    // MSVC defines this for /fsanitize=address and provides no UBSan or TSan.
    constexpr bool kAddressSanitizerActive = true;
    constexpr bool kUndefinedBehaviorSanitizerActive = false;
    constexpr bool kThreadSanitizerActive = false;
#else
    constexpr bool kAddressSanitizerActive = false;
    constexpr bool kUndefinedBehaviorSanitizerActive = false;
    constexpr bool kThreadSanitizerActive = false;
#endif
  } // namespace

  // STATIC_REQUIRE fails the build of this test, so an uninstrumented sanitizer
  // run or an unexpectedly instrumented normal run cannot report success.
  TEST_CASE("Sanitizer profile - compiled instrumentation matches the selected build profile")
  {
    STATIC_REQUIRE(kAddressSanitizerActive == kAddressSanitizerExpected);
    STATIC_REQUIRE(kUndefinedBehaviorSanitizerActive == kUndefinedBehaviorSanitizerExpected);
    STATIC_REQUIRE(kThreadSanitizerActive == kThreadSanitizerExpected);
  }
} // namespace ao::test
