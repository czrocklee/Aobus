// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

namespace
{
  struct ExplicitRecord final
  {
    explicit constexpr ExplicitRecord(std::int32_t /*value*/) {}
  };

  // POSITIVE
  thread_local auto threadLiteral = 1;
} // namespace

void testLambdaBodyInitialization()
{
  auto ordinary = []
  {
    // POSITIVE
    auto bodyLiteral = 1;
    return bodyLiteral;
  };

  auto generic = []<typename T>(T /*input*/)
  {
    // POSITIVE
    auto genericBodyLiteral = 1;

    // NEGATIVE
    [[maybe_unused]] auto dependent = T{1};
    return genericBodyLiteral;
  };

  // NEGATIVE
  [[maybe_unused]] auto result = ordinary() + generic(1) + generic(ExplicitRecord{1});
}

void testThreadLocalInitialization()
{
  // POSITIVE
  thread_local auto localThreadLiteral = 1;
  ++localThreadLiteral;
  ++threadLiteral;
}

template<typename T>
struct Partial;

template<typename T>
struct Partial<T*> final
{
  // POSITIVE
  static constexpr auto kLiteral = 1;

  // NEGATIVE
  static constexpr auto kDependent = T{1};

  template<typename U>
  static void initializeMemberTemplate()
  {
    // POSITIVE
    static auto memberLiteral = 1;

    // NEGATIVE
    [[maybe_unused]] auto memberDependent = U{1};
    ++memberLiteral;
  }
};

static_assert(Partial<std::int32_t*>::kLiteral == 1);
static_assert(sizeof(Partial<ExplicitRecord*>::kDependent) == sizeof(ExplicitRecord));
template void Partial<std::int32_t*>::initializeMemberTemplate<std::int32_t>();
template void Partial<ExplicitRecord*>::initializeMemberTemplate<ExplicitRecord>();
