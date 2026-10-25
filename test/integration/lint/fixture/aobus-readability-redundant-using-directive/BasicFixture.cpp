// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace ao
{
  // POSITIVE
  using namespace ao;

  namespace rt
  {
    // POSITIVE
    using namespace ao;
    // POSITIVE
    using namespace ao::rt;

    namespace test
    {
      // POSITIVE
      using namespace ao::rt;
    }
  }
}

namespace other
{
  // NEGATIVE
  using namespace ao;
}

namespace ao
{
  void func()
  {
    // POSITIVE
    using namespace ao;
  }
}

void globalFunc()
{
  // NEGATIVE
  using namespace ao;
}
