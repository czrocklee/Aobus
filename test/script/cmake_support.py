"""Select the governed CMake for tooling fixtures without a native build shell."""

import functools
import os

from ao.core import winui

_WINDOWS_CMAKE_COMPONENT = "Microsoft.VisualStudio.Component.VC.CMake.Project"


@functools.cache
def cmake_executable() -> str:
    """Return the governed CMake launcher, caching a successful discovery per interpreter."""
    if os.name != "nt":
        return "cmake"
    installation = winui.visual_studio_installation(component=_WINDOWS_CMAKE_COMPONENT)
    if installation is None:
        raise RuntimeError(f"CMake fixtures require the configured Visual Studio component {_WINDOWS_CMAKE_COMPONENT}")
    executable = winui.bundled_cmake(installation)
    if not executable.is_file():
        raise RuntimeError(f"The Visual Studio bundled CMake is missing: {executable}")
    return str(executable)
