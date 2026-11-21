// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Compare the requested Bazel profile with the compiler's instrumentation.
#ifdef __has_feature
static_assert(__has_feature(address_sanitizer) == AOBUS_EXPECT_ADDRESS_SANITIZER);
static_assert(__has_feature(undefined_behavior_sanitizer) == AOBUS_EXPECT_UNDEFINED_BEHAVIOR_SANITIZER);
static_assert(__has_feature(thread_sanitizer) == AOBUS_EXPECT_THREAD_SANITIZER);
#else
// MSVC exposes ASan through __SANITIZE_ADDRESS__ and has no UBSan or TSan.
#ifdef __SANITIZE_ADDRESS__
static_assert(AOBUS_EXPECT_ADDRESS_SANITIZER);
#else
static_assert(!AOBUS_EXPECT_ADDRESS_SANITIZER);
#endif
static_assert(!AOBUS_EXPECT_UNDEFINED_BEHAVIOR_SANITIZER && !AOBUS_EXPECT_THREAD_SANITIZER);
#endif

int main()
{
  return 0;
}
