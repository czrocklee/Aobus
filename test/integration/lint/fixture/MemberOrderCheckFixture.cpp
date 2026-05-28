// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

// NEGATIVE: Correct order (public -> protected -> private)
class CorrectOrderDemo
{
public:
  void doPublic();

protected:
  void doProtected();

private:
  std::int32_t _val1;
};

class OrderDemo
{
private:
  std::int32_t _val1;

  // POSITIVE
public:
  void doPublic();
};

class StructOrderDemo
{
protected:
  std::int32_t val1;

  // POSITIVE
public:
  void doPublic();
};
