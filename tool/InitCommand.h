// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "BasicCommand.h"

#include <rs/core/MusicLibrary.h>

class InitCommand : public BasicCommand
{
public:
  explicit InitCommand(rs::core::MusicLibrary& ml);
};
