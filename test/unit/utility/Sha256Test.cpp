// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/Sha256.h>

#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace ao::utility::test
{
  namespace
  {
    std::string hexOf(std::string_view const text)
    {
      return sha256Hex(computeSha256(bytes::view(text)));
    }
  } // namespace

  TEST_CASE("computeSha256 - reproduces published vectors through the wrapper", "[utility][unit][hash]")
  {
    // FIPS 180-4 / RFC 6234 test vectors, which is what makes this a check of the
    // wrapper rather than of itself.
    CHECK(hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  }

  TEST_CASE("sha256Hex - one lower-case fixed-width spelling per digest", "[utility][unit][hash]")
  {
    auto const digest = computeSha256(bytes::view(std::string_view{"abc"}));
    auto const text = sha256Hex(digest);

    CHECK(text.size() == Sha256Digest::kHexLength);
    CHECK(text == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    for (auto const character : text)
    {
      CHECK(((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')));
    }
  }

  TEST_CASE("parseSha256Hex - accepts only the form sha256Hex produces", "[utility][unit][hash]")
  {
    auto const digest = computeSha256(bytes::view(std::string_view{"abc"}));
    auto const text = sha256Hex(digest);

    auto const optParsed = parseSha256Hex(text);
    REQUIRE(optParsed);
    CHECK(*optParsed == digest);

    SECTION("an uppercase spelling of a real digest is refused")
    {
      auto upper = text;

      for (auto& character : upper)
      {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
      }

      CHECK_FALSE(parseSha256Hex(upper));
    }

    SECTION("a short, long, or non-hexadecimal spelling is refused")
    {
      CHECK_FALSE(parseSha256Hex(text.substr(0, Sha256Digest::kHexLength - 1)));
      CHECK_FALSE(parseSha256Hex(text + "0"));
      CHECK_FALSE(parseSha256Hex(std::string(Sha256Digest::kHexLength, 'z')));
      CHECK_FALSE(parseSha256Hex(""));
    }
  }

  TEST_CASE("computeSha256 - an empty input hashes rather than zeroing", "[utility][unit][hash]")
  {
    auto const empty = computeSha256({});
    CHECK(empty != Sha256Digest{});
    CHECK(sha256Hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  }

  TEST_CASE("Sha256Digest - orders by its bytes, which is what an ascending emission depends on",
            "[utility][unit][hash]")
  {
    auto lower = Sha256Digest{};
    auto higher = Sha256Digest{};
    higher.bytes[0] = std::byte{0x01};

    CHECK(lower < higher);
    CHECK(higher > lower);
    CHECK(lower == Sha256Digest{});
  }
} // namespace ao::utility::test
