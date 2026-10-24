// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace retained::sourceUses
{
  inline constexpr int value = 1;
  // POSITIVE
  constexpr int argument(int selected = retained::sourceUses::value)
  {
    return selected;
  }

  struct Aggregate
  {
    // POSITIVE
    int selected = retained::sourceUses::value;
  };

  constexpr int function(int selected)
  {
    return selected;
  }
  template<typename T>
  struct Host
  {
    // POSITIVE
    using Type = decltype(retained::sourceUses::function(1));
  };

  constexpr int capturedValue()
  {
    return [value = 2]
    {
      // NEGATIVE - shortening would select the init-capture, changing the value.
      return retained::sourceUses::value;
    }();
  }
  static_assert(capturedValue() == 1);

  constexpr int unrelatedCapture()
  {
    return [unrelated = 2]
    {
      // POSITIVE
      return retained::sourceUses::value + unrelated;
    }();
  }
  static_assert(unrelatedCapture() == 3);
}

// Uses outside the declaration namespace must not change declaration-site lookup.
static_assert(retained::sourceUses::argument() == 1);
static_assert(retained::sourceUses::Aggregate{}.selected == 1);
static_assert(__is_same(retained::sourceUses::Host<void>::Type, int));

namespace externalUses
{
  template<int... Indices>
  constexpr int defaultArguments()
  {
    return ((retained::sourceUses::argument() + Indices * 0) + ...);
  }
  template<int... Indices>
  constexpr int defaultInitializers()
  {
    return ((retained::sourceUses::Aggregate{}.selected + Indices * 0) + ...);
  }

  // Exercise shared default expressions without a machine-dependent timer assertion.
  static_assert(defaultArguments<0,
                                 1,
                                 2,
                                 3,
                                 4,
                                 5,
                                 6,
                                 7,
                                 8,
                                 9,
                                 10,
                                 11,
                                 12,
                                 13,
                                 14,
                                 15,
                                 16,
                                 17,
                                 18,
                                 19,
                                 20,
                                 21,
                                 22,
                                 23,
                                 24,
                                 25,
                                 26,
                                 27,
                                 28,
                                 29,
                                 30,
                                 31>() == 32);
  static_assert(defaultInitializers<0,
                                    1,
                                    2,
                                    3,
                                    4,
                                    5,
                                    6,
                                    7,
                                    8,
                                    9,
                                    10,
                                    11,
                                    12,
                                    13,
                                    14,
                                    15,
                                    16,
                                    17,
                                    18,
                                    19,
                                    20,
                                    21,
                                    22,
                                    23,
                                    24,
                                    25,
                                    26,
                                    27,
                                    28,
                                    29,
                                    30,
                                    31>() == 32);
}
