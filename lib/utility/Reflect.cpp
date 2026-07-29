// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/yaml/Reflect.h>

#include <ao/yaml/RymlAdapter.h>

#include <string_view>

namespace ao::yaml::detail
{
  void writeQuotedString(ryml::NodeRef node, std::string_view value)
  {
    node |= ryml::VAL;
    node |= ryml::VAL_DQUO;
    setValue(node, value);
  }

  void writePlainScalar(ryml::NodeRef node, std::string_view value)
  {
    node |= ryml::VAL;
    node |= ryml::VAL_PLAIN;
    setValue(node, value);
  }
} // namespace ao::yaml::detail
