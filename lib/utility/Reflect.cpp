// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/yaml/Reflect.h>

#include <ao/yaml/RymlAdapter.h>

#include <string_view>

namespace ao::yaml::detail
{
  void writeQuotedString(ryml::NodeRef node, std::string_view value)
  {
    setValue(node, value);
    node.set_val_style(ryml::VAL_DQUO);
  }

  void writePlainScalar(ryml::NodeRef node, std::string_view value)
  {
    setValue(node, value);
    node.set_val_style(ryml::VAL_PLAIN);
  }
} // namespace ao::yaml::detail
