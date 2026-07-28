// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/MemoryRandomAccessStream.h>

#include <catch2/catch_test_macros.hpp>
#include <objidl.h>
#include <shcore.h>
#include <winrt/base.h>

#include <array>
#include <cstddef>
#include <utility>

namespace ao::winui::test
{
  namespace
  {
    class Apartment final
    {
    public:
      Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }

      ~Apartment() { winrt::uninit_apartment(); }

      Apartment(Apartment const&) = delete;
      Apartment& operator=(Apartment const&) = delete;
      Apartment(Apartment&&) = delete;
      Apartment& operator=(Apartment&&) = delete;
    };
  } // namespace

  TEST_CASE("MemoryRandomAccessStream - prepared storage wraps exact bytes without another payload write",
            "[runtime][unit][resource-byte]")
  {
    auto apartment = Apartment{};
    auto const expected = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    auto prepared = prepareMemoryRandomAccessStream(expected);

    REQUIRE(prepared);
    auto randomAccessStream = makeMemoryRandomAccessStream(std::move(prepared));
    REQUIRE(randomAccessStream);

    auto stream = winrt::com_ptr<IStream>{};
    winrt::check_hresult(::CreateStreamOverRandomAccessStream(
      winrt::get_unknown(randomAccessStream), __uuidof(IStream), stream.put_void()));
    auto actual = std::array<std::byte, expected.size()>{};
    ULONG readCount = 0;
    winrt::check_hresult(stream->Read(actual.data(), static_cast<ULONG>(actual.size()), &readCount));

    CHECK(readCount == actual.size());
    CHECK(actual == expected);
  }
} // namespace ao::winui::test
