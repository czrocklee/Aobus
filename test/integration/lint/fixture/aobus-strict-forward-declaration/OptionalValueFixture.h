// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TemplateValueTypes.h"
#include "WeakOnlyTypes.h"

#include <optional>

class TestOptionalValueDependency
{
  std::optional<OptionalValueType> value();

  // NEGATIVE OptionalValueType must be complete for the by-value optional return.
  void consume(OptionalValueType const& value);
};

class TestOptionalPointerDependency
{
  std::optional<TargetBadRawPtr*> value();

  // POSITIVE
  void consume(TargetBadRawPtr const& value);
};
