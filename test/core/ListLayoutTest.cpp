/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <rs/core/ListLayout.h>
#include <test/core/TestUtils.h>

#include <vector>

namespace
{
  using namespace test;
  using rs::core::ListHeader;
  using rs::core::ListView;

  // Helper to create a ListView for testing
  std::vector<char> createListData(std::uint64_t trackIdsCount = 0,
                                   std::uint32_t nameId = 0,
                                   std::uint32_t descId = 0,
                                   std::uint32_t filterId = 0,
                                   std::uint8_t flags = 0,
                                   std::string_view name = "",
                                   std::string_view desc = "")
  {
    ListHeader h{};
    h.trackIdsOffset = 0;
    h.trackIdsCount = trackIdsCount;
    h.nameId = nameId;
    h.descId = descId;
    h.filterId = filterId;
    h.flags = flags;

    // Name and description are stored after the header
    h.nameOffset = 0;
    h.nameLen = static_cast<std::uint16_t>(name.size());
    h.descOffset = static_cast<std::uint16_t>(name.size() + 1); // after name + null
    h.descLen = static_cast<std::uint16_t>(desc.size());

    auto data = serializeHeader(h);

    // Add name + null
    if (!name.empty())
    {
      appendString(data, name);
    }

    // Add description + null
    if (!desc.empty())
    {
      appendString(data, desc);
    }

    return data;
  }

  TEST_CASE("ListHeader - Size and Alignment")
  {
    CHECK(sizeof(ListHeader) == 48);
    CHECK(alignof(ListHeader) == 8);
  }

  TEST_CASE("ListHeader - Field Offsets")
  {
    ListHeader h{};

    // Check 8-byte section
    CHECK(reinterpret_cast<char*>(&h.trackIdsOffset) - reinterpret_cast<char*>(&h) == 0);
    CHECK(reinterpret_cast<char*>(&h.trackIdsCount) - reinterpret_cast<char*>(&h) == 8);

    // Check 4-byte section
    CHECK(reinterpret_cast<char*>(&h.nameId) - reinterpret_cast<char*>(&h) == 16);
    CHECK(reinterpret_cast<char*>(&h.descId) - reinterpret_cast<char*>(&h) == 20);
    CHECK(reinterpret_cast<char*>(&h.filterId) - reinterpret_cast<char*>(&h) == 24);

    // Check 2-byte section
    CHECK(reinterpret_cast<char*>(&h.nameOffset) - reinterpret_cast<char*>(&h) == 28);
    CHECK(reinterpret_cast<char*>(&h.nameLen) - reinterpret_cast<char*>(&h) == 30);
    CHECK(reinterpret_cast<char*>(&h.descOffset) - reinterpret_cast<char*>(&h) == 32);
    CHECK(reinterpret_cast<char*>(&h.descLen) - reinterpret_cast<char*>(&h) == 34);

    // Check 1-byte section
    CHECK(reinterpret_cast<char*>(&h.flags) - reinterpret_cast<char*>(&h) == 36);
  }

  TEST_CASE("ListView - Default Constructor")
  {
    ListView view;
    CHECK(view.isValid() == false);
  }

  TEST_CASE("ListView - Construct from Data")
  {
    auto data = createListData();
    ListView view(data.data(), data.size());

    CHECK(view.isValid() == true);
    CHECK(view.header() != nullptr);
  }

  TEST_CASE("ListView - Field Accessors")
  {
    auto data = createListData(42, 1, 2, 3, 5);
    ListView view(data.data(), data.size());

    CHECK(view.trackIdsCount() == 42);
    CHECK(view.nameId() == 1);
    CHECK(view.descId() == 2);
    CHECK(view.filterId() == 3);
    CHECK(view.flags() == 5);
  }

  TEST_CASE("ListView - Name Accessor")
  {
    auto data = createListData(5, 0, 0, 0, 0, "My Playlist");
    ListView view(data.data(), data.size());

    CHECK(view.name() == "My Playlist");
  }

  TEST_CASE("ListView - Empty Name")
  {
    auto data = createListData(0, 0, 0, 0, 0, "");
    ListView view(data.data(), data.size());

    CHECK(view.name().empty());
  }

  TEST_CASE("ListView - Invalid Data")
  {
    // Null data
    ListView nullView(nullptr, 100);
    CHECK(nullView.isValid() == false);

    // Too small
    char smallData[10] = {};
    ListView smallView(smallData, sizeof(smallData));
    CHECK(smallView.isValid() == false);
  }

  TEST_CASE("ListView - Zero Values")
  {
    auto data = createListData(0, 0, 0, 0, 0);
    ListView view(data.data(), data.size());

    CHECK(view.trackIdsCount() == 0);
    CHECK(view.nameId() == 0);
    CHECK(view.descId() == 0);
    CHECK(view.filterId() == 0);
    CHECK(view.flags() == 0);
    CHECK(view.name().empty());
  }

  TEST_CASE("ListView - Large Values")
  {
    auto data = createListData(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF);
    ListView view(data.data(), data.size());

    CHECK(view.trackIdsCount() == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(view.nameId() == 0xFFFFFFFF);
    CHECK(view.descId() == 0xFFFFFFFF);
    CHECK(view.filterId() == 0xFFFFFFFF);
    CHECK(view.flags() == 0xFF);
  }

} // anonymous namespace
