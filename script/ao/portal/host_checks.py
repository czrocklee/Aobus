"""Read-only native host prerequisite inspection."""

from ao.core.proc import die
from ao.portal import winui


def check_winui(*, build_only: bool = False) -> int:
    checks = winui.inspect_host(include_runtime=not build_only)
    for check in checks:
        marker = "ok" if check.ok else "FAIL" if check.required else "warn"
        print(f"[{marker:>4}] {check.label}: {check.detail}")
    if any(check.required and not check.ok for check in checks):
        raise die("WinUI prerequisites are incomplete.")
    print("WinUI build prerequisites are ready.")
    return 0
