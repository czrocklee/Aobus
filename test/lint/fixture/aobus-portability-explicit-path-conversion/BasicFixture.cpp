// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <filesystem>
#include <string>

void ambiguousTextBoundary(std::filesystem::path const& path)
{
  // POSITIVE
  [[maybe_unused]] auto const text = path.string();

  // POSITIVE
  [[maybe_unused]] auto const genericText = path.generic_string();
}

void explicitNativeBoundary(std::filesystem::path const& path)
{
  // NEGATIVE - native() preserves the platform-native path representation.
  [[maybe_unused]] auto const native = path.native();
}

void explicitUtf8TemplateBoundary(std::filesystem::path const& path)
{
  // NEGATIVE - char8_t selects an explicit UTF-8 result encoding.
  [[maybe_unused]] auto const text = path.string<char8_t>();

  // NEGATIVE - generic char8_t text also selects UTF-8 and generic separators.
  [[maybe_unused]] auto const genericText = path.generic_string<char8_t>();
}

void ambiguousPathConstruction(std::string const& text)
{
  // POSITIVE
  [[maybe_unused]] auto const path = std::filesystem::path{text};
}

void explicitPathConstruction(std::u8string const& utf8, std::wstring const& native)
{
  // NEGATIVE - char8_t makes the input encoding explicit.
  [[maybe_unused]] auto const utf8Path = std::filesystem::path{utf8};

  // NEGATIVE - wide text is the native Windows path representation.
  [[maybe_unused]] auto const nativePath = std::filesystem::path{native};

  // NEGATIVE - an ASCII literal has the same bytes in every supported encoding.
  [[maybe_unused]] auto const literalPath = std::filesystem::path{"music.flac"};
}

void ambiguousTemporaryBoundary()
{
  // POSITIVE
  [[maybe_unused]] auto const text = std::filesystem::path{"music.flac"}.string();
}

void unrelatedString(std::string const& text)
{
  // NEGATIVE - the checker is semantic and does not reject other string APIs.
  [[maybe_unused]] auto const copy = std::string{text};
}

struct Token final
{
  std::string string() const;
};

void unrelatedStringMethod(Token const& token)
{
  // NEGATIVE - only std::filesystem::path conversions belong to this policy.
  [[maybe_unused]] auto const text = token.string();
}
