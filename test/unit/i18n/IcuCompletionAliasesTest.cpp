// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/IcuCompletionAliases.h>

#include <ao/Error.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>

#include <catch2/catch_test_macros.hpp>

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
      auto const result = policy.makeAliasesInto(aliases, text);
      REQUIRE(result);
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

  TEST_CASE("IcuCompletionAliases - unsupported and short source shapes produce no aliases",
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

    auto malformedRes = policyPtr->makeAliasesInto(aliases, "bad\xFFtext");
    REQUIRE_FALSE(malformedRes);
    CHECK(malformedRes.error().code == Error::Code::InvalidInput);
    CHECK(aliases.empty());

    auto nonNfcRes = policyPtr->makeAliasesInto(aliases, "Cafe\u0301");
    REQUIRE_FALSE(nonNfcRes);
    CHECK(nonNfcRes.error().code == Error::Code::InvalidInput);
    CHECK(aliases.empty());

    auto embeddedNulRes = policyPtr->makeAliasesInto(aliases, std::string_view{"周\0杰", 7});
    REQUIRE_FALSE(embeddedNulRes);
    CHECK(embeddedNulRes.error().code == Error::Code::InvalidInput);
    CHECK(aliases.empty());
  }
} // namespace ao::i18n::test
