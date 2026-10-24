"""Load portal commands from their lightweight identity registry.

Each module declares the matching NAME, REQUIRES_BUILD_ENV and parser
registration. Phase reporting reads the registry without importing this package.
"""

from importlib import import_module

from ..core.command_registry import COMMAND_MODULE_NAMES

COMMAND_MODULES = tuple(import_module(f"{__name__}.{name}") for name in COMMAND_MODULE_NAMES)
for _name, _module in zip(COMMAND_MODULE_NAMES, COMMAND_MODULES, strict=True):
    if _module.NAME != _name.replace("_", "-"):
        raise ValueError("portal command name differs from its registry identity")
