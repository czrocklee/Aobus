// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <array>
#include <string_view>
#include <utility>

struct CodecName
{
  std::string_view codec;
};

void positiveCases()
{
  // POSITIVE
  constexpr std::array codecs{CodecName{.codec = "mp3"}, CodecName{.codec = "aac"}};

  // POSITIVE
  constexpr std::array<CodecName, 2> codecs3{{{.codec = "mp3"}, {.codec = "aac"}}};
}

void negativeCases()
{
  // NEGATIVE
  constexpr auto codecs = std::to_array<CodecName>({
    {.codec = "mp3"},
    {.codec = "aac"},
  });

  // NEGATIVE
  std::array<int, 10> emptyBuf{};

  // NEGATIVE
  constexpr std::array simple{"a", "b", "c"};
}
