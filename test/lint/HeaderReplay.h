// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

// This two-statement body deliberately requires a header-main-only diagnostic.
inline int headerReplayValue(HeaderReplayInput const& input)
{
  int value = input.value + AOBUS_HEADER_REPLAY_VALUE;
  return value;
}

// The late, unused include independently exercises main-file PP callbacks and
// the upstream include-cleaner; this manual canary is not shipping code.
#include "HeaderReplayDetail.h"
