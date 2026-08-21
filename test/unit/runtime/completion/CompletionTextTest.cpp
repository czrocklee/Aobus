// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/CompletionText.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  TEST_CASE("findCompletionWordPrefixInsensitive finds value and ASCII-delimited word prefixes",
            "[runtime][unit][completion]")
  {
    CHECK(findCompletionWordPrefixInsensitive("Trevor Pinnock", "tre") == std::optional<std::size_t>{0});

    for (auto const* const candidate : {
           "Trevor Pinnock",
           "Trevor-Pinnock",
           "Trevor/Pinnock",
           "Trevor.Pinnock",
           "Trevor:Pinnock",
           "Trevor_Pinnock",
           "Trevor(Pinnock",
         })
    {
      CHECK(findCompletionWordPrefixInsensitive(candidate, "pIn") == std::optional<std::size_t>{7});
    }
  }

  TEST_CASE("findCompletionWordPrefixInsensitive does not split ASCII or UTF-8 words", "[runtime][unit][completion]")
  {
    CHECK_FALSE(findCompletionWordPrefixInsensitive("Trevor Pinnock", "innock"));
    CHECK_FALSE(findCompletionWordPrefixInsensitive("TrevorPinnock", "pinnock"));
    CHECK_FALSE(findCompletionWordPrefixInsensitive("éPinnock", "pinnock"));
    CHECK_FALSE(findCompletionWordPrefixInsensitive("éclair", std::string_view{"\xA9", 1}));
    CHECK(findCompletionWordPrefixInsensitive("é-Pinnock", "pinnock") == std::optional<std::size_t>{3});
  }

  TEST_CASE("makeCompletionAliasPrefixKey compacts only ASCII letters and digits", "[runtime][unit][completion]")
  {
    CHECK(makeCompletionAliasPrefixKey(" Zhou-Jie_Lun 99! ") == std::optional<std::string>{"zhoujielun99"});
    CHECK(makeCompletionAliasPrefixKey("A-12") == std::optional<std::string>{"a12"});
    CHECK_FALSE(makeCompletionAliasPrefixKey("A-1"));
    CHECK_FALSE(makeCompletionAliasPrefixKey("---"));
    CHECK_FALSE(makeCompletionAliasPrefixKey("abc周"));
  }
} // namespace ao::rt::test
