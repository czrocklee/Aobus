// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::utility::test
{
  namespace
  {
    struct Dummy final
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
  }

  TEST_CASE("ByteView - View creation and layout casts", "[utility][unit][byte_view]")
  {
    SECTION("bytes::view overloads")
    {
      auto const str = std::string{"Hello"};
      auto const sView = std::string_view{str};
      auto const dummy = Dummy{.a = 1, .b = 2};
      auto arr = std::to_array<Dummy>({{.a = 1, .b = 2}, {.a = 3, .b = 4}});
      auto vec = std::vector{std::byte{1}, std::byte{2}};
      auto const vecConst = std::vector{std::byte{1}, std::byte{2}};

      auto v1 = bytes::view(static_cast<void const*>(str.data()), str.size());
      CHECK(v1.size() == 5);

      auto v2 = bytes::view(static_cast<void*>(vec.data()), vec.size());
      CHECK(v2.size() == 2);

      auto v3 = bytes::view(vec.data(), vec.size());
      CHECK(v3.size() == 2);

      auto v4 = bytes::view(&dummy);
      CHECK(v4.size() == sizeof(Dummy));

      auto v5 = bytes::view(std::span<Dummy>{arr});
      CHECK(v5.size() == 2 * sizeof(Dummy));

      auto v6 = bytes::view(dummy);
      CHECK(v6.size() == sizeof(Dummy));

      auto v7 = bytes::view(sView);
      CHECK(v7.size() == 5);

      auto v8 = bytes::view(str);
      CHECK(v8.size() == 5);

      CHECK(bytes::stringView(v8) == "Hello");
      CHECK(bytes::stringView(std::span<std::byte const>{}).empty());
    }

    SECTION("layout:: functions")
    {
      auto dummy = Dummy{.a = 42, .b = 84};
      auto const span = bytes::view(dummy);
      auto spanMut = std::span<std::byte>{reinterpret_cast<std::byte*>(&dummy), sizeof(Dummy)};

      auto const* ptr = layout::view<Dummy>(span);
      CHECK(ptr->a == 42);

      auto* ptrMut = layout::viewMutable<Dummy>(spanMut);
      ptrMut->a = 10;
      CHECK(dummy.a == 10);

      auto const* p = layout::asPtr<Dummy>(span);
      CHECK(p->a == 10);

      auto* legacy = layout::asLegacyPtr<Dummy>(span);
      CHECK(legacy->a == 10);

      auto const* constDummy = &dummy;
      auto* legacy2 = layout::asLegacyPtr<Dummy>(constDummy);
      CHECK(legacy2->a == 10);

      CHECK(layout::size32(span) == static_cast<std::uint32_t>(sizeof(Dummy)));

      auto* mut = layout::asMutablePtr<Dummy>(spanMut);
      mut->b = 20;
      CHECK(dummy.b == 20);

      auto arr = std::to_array<Dummy>({{.a = 1, .b = 2}, {.a = 3, .b = 4}});
      auto arrSpan = bytes::view(std::span<Dummy>{arr});
      auto arrSpanMut = std::span<std::byte>{reinterpret_cast<std::byte*>(arr.data()), 2 * sizeof(Dummy)};

      auto varr = layout::viewArray<Dummy>(arrSpan);
      CHECK(varr.size() == 2);
      CHECK(varr[1].a == 3);

      auto varrMut = layout::viewArrayMutable<Dummy>(arrSpanMut);
      CHECK(varrMut.size() == 2);
      varrMut[1].a = 5;
      CHECK(arr[1].a == 5);

      struct Outer
      {
        Dummy d;
        Dummy d2;
      };
      auto out = Outer{.d = {.a = 1, .b = 2}, .d2 = {.a = 3, .b = 4}};
      auto const* inner = layout::viewAt<Dummy>(&out, sizeof(Dummy));
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