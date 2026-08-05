// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace ao
{
  template<typename T = void>
  class Result
  {
  public:
    Result() = default;
  };
} // namespace ao

using ao::Result;

class TestClass
{
  // POSITIVE
  Result<int> _invalidMember;

  // NEGATIVE
  Result<int> _validMemberRes;

  // NEGATIVE
  Result<int> _result;

  // POSITIVE
  Result<int> _res;
};

void acceptResult(
  // POSITIVE
  Result<int> badParam,
  // NEGATIVE
  Result<int> goodParamRes)
{
  (void)badParam;
  (void)goodParamRes;
}

void testResultNaming()
{
  // POSITIVE
  [[maybe_unused]] auto invalidLocal = Result<int>{};

  // NEGATIVE
  [[maybe_unused]] auto validLocalRes = Result<int>{};

  // NEGATIVE
  [[maybe_unused]] Result<void> res;

  // NEGATIVE
  [[maybe_unused]] Result<void> result;

  // NEGATIVE
  [[maybe_unused]] Result<void> openRes;

  // POSITIVE
  [[maybe_unused]] Result<void> features;

  // POSITIVE
  [[maybe_unused]] Result<void> invalidValue;
}
