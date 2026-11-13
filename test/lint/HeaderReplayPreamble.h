// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

struct HeaderReplayInput final
{
  int value;
};

#define AOBUS_HEADER_REPLAY_VALUE 17

// Source TUs receive the aggregate; header-main analysis owns its declaration.
#ifndef AOBUS_LINT_HEADER_MAIN
#include "HeaderReplay.h"
#endif
