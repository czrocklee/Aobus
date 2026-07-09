// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ao::utility::test
{
  namespace
  {
    struct LayoutRecord final
    {
      std::uint32_t a;
      std::uint32_t b;
    };

    struct Base
    {
      Base() = default;
      virtual ~Base() = default;
      Base(Base const&) = default;
      Base& operator=(Base const&) = default;
      Base(Base&&) = default;
      Base& operator=(Base&&) = default;
    };
    struct Derived : Base
    {
      std::int32_t x = 0;
    };
  } // namespace

  TEST_CASE("ByteView - creates byte views and performs layout casts", "[utility][unit][byte-view]")
  {
    SECTION("bytes::view overloads")
    {
      auto const str = std::string{"Hello"};
      auto const sView = std::string_view{str};
      auto const record = LayoutRecord{.a = 1, .b = 2};
      auto arr = std::to_array<LayoutRecord>({{.a = 1, .b = 2}, {.a = 3, .b = 4}});
      auto vec = std::vector{std::byte{1}, std::byte{2}};
      auto const vecConst = std::vector{std::byte{1}, std::byte{2}};

      auto v1 = bytes::view(static_cast<void const*>(str.data()), str.size());
      CHECK(v1.size() == 5);

      auto v2 = bytes::view(static_cast<void*>(vec.data()), vec.size());
      CHECK(v2.size() == 2);

      auto v3 = bytes::view(vec.data(), vec.size());
      CHECK(v3.size() == 2);

      auto v4 = bytes::view(&record);
      CHECK(v4.size() == sizeof(LayoutRecord));

      auto v5 = bytes::view(std::span<LayoutRecord>{arr});
      CHECK(v5.size() == 2 * sizeof(LayoutRecord));

      auto v6 = bytes::view(record);
      CHECK(v6.size() == sizeof(LayoutRecord));

      auto v7 = bytes::view(sView);
      CHECK(v7.size() == 5);

      auto v8 = bytes::view(str);
      CHECK(v8.size() == 5);

      CHECK(bytes::stringView(v8) == "Hello");
      CHECK(bytes::stringView(std::span<std::byte const>{}).empty());

      auto const* const unsignedChars = bytes::unsignedCharData(v8);
      CHECK(static_cast<char>(unsignedChars[0]) == 'H');
    }

    SECTION("bytes::tryLayout / requireLayout are always checked")
    {
      auto const record = LayoutRecord{.a = 7, .b = 9};
      auto const full = bytes::view(record); // aligned, exact size

      CHECK(bytes::tryLayout<LayoutRecord>(full) != nullptr);
      CHECK(bytes::requireLayout<LayoutRecord>(full)->a == 7);

      // Too short for the target type.
      auto const shortSpan = full.subspan(0, sizeof(LayoutRecord) - 1);
      CHECK(bytes::tryLayout<LayoutRecord>(shortSpan) == nullptr);
      CHECK_THROWS_AS(bytes::requireLayout<LayoutRecord>(shortSpan), std::out_of_range);

      // Misaligned for the target type (offset by one from an aligned buffer).
      alignas(LayoutRecord) auto aligned = std::array<std::byte, sizeof(LayoutRecord) + alignof(LayoutRecord)>{};
      auto const misaligned = std::span<std::byte const>{aligned.data() + 1, sizeof(LayoutRecord)};
      CHECK(bytes::tryLayout<LayoutRecord>(misaligned) == nullptr);
      CHECK_THROWS_AS(bytes::requireLayout<LayoutRecord>(misaligned), std::out_of_range);
    }

    SECTION("layout:: functions")
    {
      auto record = LayoutRecord{.a = 42, .b = 84};
      auto const span = bytes::view(record);
      auto spanMut = std::span<std::byte>{reinterpret_cast<std::byte*>(&record), sizeof(LayoutRecord)};

      auto const* ptr = layout::view<LayoutRecord>(span);
      CHECK(ptr->a == 42);

      auto* ptrMut = layout::viewMutable<LayoutRecord>(spanMut);
      ptrMut->a = 10;
      CHECK(record.a == 10);

      auto const* p = layout::asPtr<LayoutRecord>(span);
      CHECK(p->a == 10);

      auto* legacy = layout::asLegacyPtr<LayoutRecord>(span);
      CHECK(legacy->a == 10);

      auto const* constRecord = &record;
      auto* legacy2 = layout::asLegacyPtr<LayoutRecord>(constRecord);
      CHECK(legacy2->a == 10);

      CHECK(layout::size32(span) == static_cast<std::uint32_t>(sizeof(LayoutRecord)));

      auto* mut = layout::asMutablePtr<LayoutRecord>(spanMut);
      mut->b = 20;
      CHECK(record.b == 20);

      auto arr = std::to_array<LayoutRecord>({{.a = 1, .b = 2}, {.a = 3, .b = 4}});
      auto arrSpan = bytes::view(std::span<LayoutRecord>{arr});
      auto arrSpanMut = std::span<std::byte>{reinterpret_cast<std::byte*>(arr.data()), 2 * sizeof(LayoutRecord)};

      auto varr = layout::viewArray<LayoutRecord>(arrSpan);
      CHECK(varr.size() == 2);
      CHECK(varr[1].a == 3);

      auto varrMut = layout::viewArrayMutable<LayoutRecord>(arrSpanMut);
      CHECK(varrMut.size() == 2);
      varrMut[1].a = 5;
      CHECK(arr[1].a == 5);

      struct Outer
      {
        LayoutRecord d;
        LayoutRecord d2;
      };
      auto out = Outer{.d = {.a = 1, .b = 2}, .d2 = {.a = 3, .b = 4}};
      auto const* inner = layout::viewAt<LayoutRecord>(&out, sizeof(LayoutRecord));
      CHECK(inner->a == 3);
    }

    SECTION("uint64Parts")
    {
      std::uint64_t const val = 0x1122334455667788ULL;
      auto const [lo, hi] = uint64Parts::split(val);
      CHECK(lo == 0x55667788U);
      CHECK(hi == 0x11223344U);
      CHECK(uint64Parts::combine(lo, hi) == val);
    }

    SECTION("unsafeDowncast")
    {
      auto d = Derived{};
      Base* base = &d;
      Base& baseRef = d;
      void* voidAddr = &d;

      Derived const* derived = unsafeDowncast<Derived>(base);
      CHECK(derived == &d);

      Derived& dRef = unsafeDowncast<Derived>(baseRef);
      CHECK(&dRef == &d);

      Derived const* derivedFromVoid = unsafeDowncast<Derived>(voidAddr);
      CHECK(derivedFromVoid == &d);
    }
  }
} // namespace ao::utility::test
