// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "ComboCommand.h"
#include <rs/core/MusicLibrary.h>

class LibCommand : public ComboCommand
{
public:
  explicit LibCommand(rs::core::MusicLibrary& ml);

private:
  rs::core::MusicLibrary& _ml;
};
