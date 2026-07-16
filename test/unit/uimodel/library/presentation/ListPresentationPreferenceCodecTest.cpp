// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceCodec.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentationPreferenceCodec - round-trip preserves opaque presentation ids",
            "[uimodel][unit][presentation-preference]")
  {
    auto state = ListPresentationPreferenceState{};
    state.presentations[ListId{10}] = "plugin-preset";

    auto const document = encodeListPresentationPreferences(state);

    REQUIRE(document);
    CHECK(document->version == 1);
    REQUIRE(document->preferences.size() == 1);
    CHECK(document->preferences[0].listId == 10);
    CHECK(document->preferences[0].presentationId == "plugin-preset");

    auto const decoded = decodeListPresentationPreferences(*document);

    REQUIRE(decoded);
    REQUIRE(decoded->presentations.size() == 1);
    CHECK(decoded->presentations.at(ListId{10}) == "plugin-preset");
  }

  TEST_CASE("ListPresentationPreferenceCodec - rejects an invalid document as one object",
            "[uimodel][unit][presentation-preference]")
  {
    auto document = ListPresentationPreferenceDocument{
      .preferences =
        {
          StoredListPresentationPreference{.listId = 10, .presentationId = "albums"},
        },
    };

    SECTION("Unsupported version")
    {
      document.version = 2;
      auto const result = decodeListPresentationPreferences(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Invalid list id")
    {
      document.preferences[0].listId = kInvalidListId.raw();
      auto const result = decodeListPresentationPreferences(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Empty presentation id")
    {
      document.preferences[0].presentationId.clear();
      auto const result = decodeListPresentationPreferences(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Duplicate list id")
    {
      document.preferences.push_back(document.preferences[0]);
      auto const result = decodeListPresentationPreferences(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }
  }

  TEST_CASE("ListPresentationPreferenceCodec - refuses to encode noncanonical live state",
            "[uimodel][unit][presentation-preference]")
  {
    auto state = ListPresentationPreferenceState{};

    SECTION("Invalid list id")
    {
      state.presentations[kInvalidListId] = "albums";
    }

    SECTION("Empty presentation id")
    {
      state.presentations[ListId{10}] = "";
    }

    auto const result = encodeListPresentationPreferences(state);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
  }
} // namespace ao::uimodel::test
