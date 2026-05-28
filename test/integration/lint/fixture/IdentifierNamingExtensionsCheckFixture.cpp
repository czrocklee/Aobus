// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

class NamingDemo
{
public:
  // POSITIVE
  std::int32_t memberValue = 10;

  // POSITIVE
  std::int32_t _member_invalid = 5;

  // POSITIVE
  std::int32_t _InvalidTitle = 6;

  // NEGATIVE
  std::int32_t _conformingValue = 20;
};

struct StructNamingDemo
{
  // POSITIVE
  std::int32_t _invalidStructVal = 5;

  // NEGATIVE
  std::int32_t validStructVal = 10;
};

struct ClassLikeStruct
{
private:
  std::int32_t _privateVal = 0; // NEGATIVE: Exempt due to private member (considered class-like)
};
