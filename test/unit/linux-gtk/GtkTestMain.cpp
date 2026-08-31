// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_session.hpp>
#include <glib.h>

namespace
{
  void setDefaultEnv(char const* name, char const* value)
  {
    if (::g_getenv(name) == nullptr)
    {
      ::g_setenv(name, value, TRUE);
    }
  }
} // namespace

int main(int argc, char* argv[])
{
  // These fallback defaults cover direct binary launches. The ao runner and
  // CTest set them before process startup because GTK can select some backends
  // before this main() begins:
  // - GTK_A11Y=test selects the in-process accessibility backend that
  //   gtk_test_accessible_check_property() (see GtkWidgetTestSupport.h)
  //   relies on.
  // - GDK_BACKEND=x11 matches the Xvfb display provided by the test runner,
  //   avoiding backend probing order issues.
  // - GDK_DISABLE=gl,vulkan together with GSK_RENDERER=cairo force the
  //   deterministic Cairo renderer, since GL/Vulkan are unavailable or slow
  //   under Xvfb.
  // An explicitly set value still wins, so debugging with a real display,
  // renderer, or accessibility backend stays possible.
  setDefaultEnv("GTK_A11Y", "test");
  setDefaultEnv("GTK_IM_MODULE", "simple");
  setDefaultEnv("GDK_BACKEND", "x11");
  setDefaultEnv("GDK_DISABLE", "gl,vulkan");
  setDefaultEnv("GSK_RENDERER", "cairo");

  return Catch::Session{}.run(argc, argv);
}
