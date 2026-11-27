// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

int main(int argc, char* argv[])
{
  auto const args = std::span{argv, static_cast<std::size_t>(argc)};

  if (args.size() != 2)
  {
    return 1;
  }

  if (auto const format = std::string_view{args[1]}; format == "json")
  {
    auto tree = ryml::parse_in_arena("{nul: old, literal: old}");
    ao::yaml::writeScalar(tree.rootref()["nul"], "a\0b");
    ao::yaml::writeScalar(tree.rootref()["literal"], R"(a\0b)");
    std::cout << ryml::emitrs_json<std::string>(tree);
  }
  else if (format == "yaml")
  {
    auto tree = ryml::parse_in_arena("block:\n  &bk !custom seed: payload\n  alias: *bk\n"
                                     "flow: {&fk !custom seed: payload, alias: *fk}\n");
    auto const key = std::string(512, '\0');
    ao::yaml::setKey(tree.rootref()["block"]["seed"], key);
    ao::yaml::setKey(tree.rootref()["flow"]["seed"], key);
    std::cout << ryml::emitrs_yaml<std::string>(tree);
  }
  else
  {
    return 1;
  }

  return 0;
}
