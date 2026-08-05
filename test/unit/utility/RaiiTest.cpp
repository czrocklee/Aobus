// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::utility::test
{
  namespace
  {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    std::int32_t g_deleteCount = 0;

    struct DummyHandle final
    {
      std::int32_t id;
    };

    void mockClose(DummyHandle* ptr)
    {
      if (ptr != nullptr)
      {
        g_deleteCount++;
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete ptr;
      }
    }
  } // namespace

  TEST_CASE("Raii - makeUniquePtr properly disposes of C-style handles", "[utility][unit]")
  {
    g_deleteCount = 0;

    SECTION("Deleter is invoked on destruction")
    {
      {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto handlePtr = makeUniquePtr<mockClose>(new DummyHandle{42});
        CHECK(handlePtr->id == 42);
        CHECK(g_deleteCount == 0);
      }
      CHECK(g_deleteCount == 1);
    }

    SECTION("Nullptr does not crash and handles safely")
    {
      {
        auto handlePtr = makeUniquePtr<mockClose>(static_cast<DummyHandle*>(nullptr));
        CHECK(handlePtr == nullptr);
      }
      CHECK(g_deleteCount == 0);
    }
  }
} // namespace ao::utility::test
