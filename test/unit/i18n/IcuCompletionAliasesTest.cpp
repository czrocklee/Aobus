// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/IcuCompletionAliases.h>

#include <ao/Error.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace ao::i18n::test
{
  namespace
  {
    std::vector<std::string> aliasesFor(rt::CompletionAliasPolicy const& policy, std::string_view const text)
    {
      auto aliases = std::vector<std::string>{};
      auto const res = policy.makeAliasesInto(aliases, text);
      REQUIRE(res);
      return aliases;
    }
  } // namespace

  TEST_CASE("IcuCompletionAliases - Kana runs produce useful romanized spellings", "[runtime][unit][completion-alias]")
  {
    auto const policyPtr = createIcuCompletionAliasPolicy();

    CHECK(aliasesFor(*policyPtr, "宇多田ヒカル") == std::vector<std::string>{"hikaru"});
    CHECK(aliasesFor(*policyPtr, "行かないで") == std::vector<std::string>{"kanaide"});
    CHECK(aliasesFor(*policyPtr, "ﾊﾝﾊﾞｰﾄ") == std::vector<std::string>{"hanbato"});
    CHECK(aliasesFor(*policyPtr, "あ゙いう") == std::vector<std::string>{"aiu"});
  }

  TEST_CASE("IcuCompletionAliases - punctuation-separated Kana runs add one combined spelling",
            "[runtime][unit][completion-alias]")
  {
    auto const policyPtr = createIcuCompletionAliasPolicy();

    CHECK(aliasesFor(*policyPtr, "ハンバート ハンバート") == std::vector<std::string>{"hanbato", "hanbatohanbato"});
    CHECK(aliasesFor(*policyPtr, "ハンバート・ハンバート") == std::vector<std::string>{"hanbato", "hanbatohanbato"});
    CHECK(aliasesFor(*policyPtr, "ハンバート の ハンバート") ==
          std::vector<std::string>{"hanbato", "hanbatonohanbato"});
  }

  TEST_CASE("IcuCompletionAliases - Han-only values use the explicit Mandarin transform",
            "[runtime][unit][completion-alias]")
  {
    auto const policyPtr = createIcuCompletionAliasPolicy();

    CHECK(aliasesFor(*policyPtr, "周杰倫") == std::vector<std::string>{"zhoujielun"});
    CHECK(aliasesFor(*policyPtr, "王菲") == std::vector<std::string>{"wangfei"});
    CHECK(aliasesFor(*policyPtr, "王妃") == std::vector<std::string>{"wangfei"});
    CHECK(aliasesFor(*policyPtr, "久石譲") == std::vector<std::string>{"jiushirang"});
    CHECK(aliasesFor(*policyPtr, "音乐") == std::vector<std::string>{"yinle"});
  }

  TEST_CASE("IcuCompletionAliases - values without useful distinct romanization produce no aliases",
            "[runtime][unit][completion-alias]")
  {
    auto const policyPtr = createIcuCompletionAliasPolicy();

    CHECK(aliasesFor(*policyPtr, "Dvořák").empty());
    CHECK(aliasesFor(*policyPtr, "plain text").empty());
    CHECK(aliasesFor(*policyPtr, "Hikaruヒカル").empty());
    CHECK(aliasesFor(*policyPtr, "の").empty());
    CHECK(aliasesFor(*policyPtr, "").empty());
  }

  TEST_CASE("IcuCompletionAliases - caller storage is replaced and admitted text is enforced",
            "[runtime][unit][completion-alias]")
  {
    auto const policyPtr = createIcuCompletionAliasPolicy();
    auto aliases = std::vector<std::string>{"stale"};

    REQUIRE(policyPtr->makeAliasesInto(aliases, "周杰倫"));
    REQUIRE(aliases == std::vector<std::string>{"zhoujielun"});

    auto const rejectedInputs = std::array{
      std::string_view{"bad\xFFtext"},
      std::string_view{"Cafe\u0301"},
      std::string_view{"周\0杰", 7},
    };

    for (auto const input : rejectedInputs)
    {
      CAPTURE(input);
      aliases = {"stale"};
      auto const rejectedRes = policyPtr->makeAliasesInto(aliases, input);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
      CHECK(aliases.empty());

      REQUIRE(policyPtr->makeAliasesInto(aliases, "王菲"));
      CHECK(aliases == std::vector<std::string>{"wangfei"});
    }
  }
} // namespace ao::i18n::test
