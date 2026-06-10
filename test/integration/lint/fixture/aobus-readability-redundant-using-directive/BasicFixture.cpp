// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace ao
{
  // POSITIVE: FIX-TO: ;
  using namespace ao;

  namespace rt
  {
    // POSITIVE: FIX-TO: ;
    using namespace ao;
    // POSITIVE: FIX-TO: ;
    using namespace ao::rt;

    namespace test
    {
      // POSITIVE: FIX-TO: ;
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
    // POSITIVE: FIX-TO: ;
    using namespace ao;
  }
}

void globalFunc()
{
  // NEGATIVE
  using namespace ao;
}
