// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>

#include <ryml.hpp>

namespace ao::uimodel
{
  struct LayoutComponentStateYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, LayoutComponentStateDocument const& document) const;
    Result<LayoutComponentStateDocument> deserialize(ryml::ConstNodeRef node,
                                                     LayoutComponentStateDocument const& seed) const;
  };
} // namespace ao::uimodel
