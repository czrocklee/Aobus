// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TemplateValueTypes.h"

#include <variant>

class TestVariantValueDependency
{
  std::variant<int, VariantValueType> _source;

  // NEGATIVE VariantValueType must be complete for the by-value variant member.
  void consume(VariantValueType const& value);
};
