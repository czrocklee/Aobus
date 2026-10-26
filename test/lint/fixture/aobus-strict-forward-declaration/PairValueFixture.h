// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TemplateValueTypes.h"

#include <utility>

class TestPairValueDependency
{
  std::pair<int, PairValueType> _entry;

  // NEGATIVE PairValueType must be complete for the by-value pair member.
  void consume(PairValueType const& value);
};
