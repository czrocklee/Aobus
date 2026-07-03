// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Output.h"

#include "CliTestSupport.h"
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <sstream>
#include <string>

namespace ao::cli::test
{
  TEST_CASE("Output - YAML quote escapes double-quoted scalars", "[cli][unit][output]")
  {
    auto value = std::string{"quote \" slash \\ newline\n tab\t return\r control "};
    value.push_back('\x01');
    value += " utf8 ";
    value += "\xC3\xA9";

    auto const quoted = yamlQuote(value);
    CHECK(quoted == "\"quote \\\" slash \\\\ newline\\n tab\\t return\\r control \\u0001 utf8 \xC3\xA9\"");

    auto tree = parseYaml("value: " + quoted);
    CHECK(yaml::scalarView(tree.rootref()["value"]) == value);
  }

  TEST_CASE("Output - JSON quote escapes string values", "[cli][unit][output]")
  {
    auto value = std::string{"quote \" slash \\ newline\n tab\t return\r control "};
    value.push_back('\x02');

    CHECK(jsonQuote(value) == "\"quote \\\" slash \\\\ newline\\n tab\\t return\\r control \\u0002\"");
    CHECK(jsonQuote("") == "\"\"");
  }

  TEST_CASE("Output - quote dispatches by output format", "[cli][unit][output]")
  {
    CHECK(quote(OutputFormat::Yaml, "a\nb") == yamlQuote("a\nb"));
    CHECK(quote(OutputFormat::Json, "a\nb") == jsonQuote("a\nb"));
  }

  TEST_CASE("Output - JSON helpers emit valid objects and arrays", "[cli][unit][output]")
  {
    auto os = std::ostringstream{};
    {
      auto object = JsonObject{os};
      object.stringField("name", "alpha");
      object.uintField("count", 2);
      object.boolField("ok", true);
      object.field("ids");
      auto ids = JsonArray{os};
      ids.element();
      os << 4;
      ids.element();
      os << 8;
    }

    CHECK(os.str() == R"({"name":"alpha","count":2,"ok":true,"ids":[4,8]})");
    auto tree = parseYaml(os.str());
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "alpha");
    CHECK(yaml::scalarView(tree.rootref()["count"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["ok"]) == "true");
    CHECK(tree.rootref()["ids"].num_children() == 2);
  }

  TEST_CASE("Output - YAML helpers emit key values and empty sequences", "[cli][unit][output]")
  {
    auto os = std::ostringstream{};
    yamlKeyValue(os, 0, "name", "alpha");
    yamlKeyValue(os, 0, "count", std::uint64_t{2});
    yamlKeyValue(os, 0, "ok", true);
    {
      auto empty = YamlSequence{os, 0, "items"};
    }

    CHECK(os.str() == "name: \"alpha\"\ncount: 2\nok: true\nitems: []\n");
    auto tree = parseYaml(os.str());
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "alpha");
    CHECK(tree.rootref()["items"].num_children() == 0);
  }

  TEST_CASE("Output - YAML sequence helper emits item prefixes", "[cli][unit][output]")
  {
    auto os = std::ostringstream{};
    {
      auto items = YamlSequence{os, 0, "items"};
      items.itemPrefix();
      os << "1\n";
      items.itemPrefix();
      os << "2\n";
    }

    CHECK(os.str() == "items:\n  - 1\n  - 2\n");
  }
} // namespace ao::cli::test
