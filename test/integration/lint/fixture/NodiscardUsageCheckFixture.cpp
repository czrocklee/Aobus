// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

// POSITIVE
[[nodiscard]] std::int32_t getForbiddenVal()
{
  return 42;
}

// POSITIVE
struct [[nodiscard]] ForbiddenStruct
{};

// POSITIVE
class [[nodiscard]] ForbiddenClass
{};

// NEGATIVE
std::int32_t getConformingVal()
{
  return 42;
}

// NEGATIVE
struct ConformingStruct
{};

// --- RAII Tests ---

// POSITIVE
class MissingNodiscardRaii
{
public:
  ~MissingNodiscardRaii() {}
  MissingNodiscardRaii(MissingNodiscardRaii const&) = delete;
};

// NEGATIVE
class [[nodiscard]] GoodRaii
{
public:
  ~GoodRaii() {}
  GoodRaii(GoodRaii const&) = delete;
};

// NEGATIVE
class NotActuallyRaii
{
public:
  ~NotActuallyRaii() {} // User-provided dtor
  // Copy ctor NOT deleted (implicit or defaulted)
};

// NEGATIVE
class AlsoNotRaii
{
public:
  // No user-provided dtor
  AlsoNotRaii(AlsoNotRaii const&) = delete;
};

// NEGATIVE
class SomeInternalImpl
{
public:
  ~SomeInternalImpl() {}
  SomeInternalImpl(SomeInternalImpl const&) = delete;
};

// NEGATIVE
class GlobalPlaybackService
{
public:
  ~GlobalPlaybackService() {}
  GlobalPlaybackService(GlobalPlaybackService const&) = delete;
};

// NEGATIVE
class MainWindow
{
public:
  ~MainWindow() {}
  MainWindow(MainWindow const&) = delete;
};

// POSITIVE
class SomeResourceToken
{
public:
  ~SomeResourceToken() {}
  SomeResourceToken(SomeResourceToken const&) = delete;
};
