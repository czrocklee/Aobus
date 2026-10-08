# Offline Windows prerequisites

This package prepares prerequisites for the unpackaged x64 WinUI application; it
does not deploy the application or implement upgrades, uninstall, public code
signing or licensing. The staged package contains six files: this README, the VC
redistributable, the Windows App Runtime installer, `prerequisites.json`,
`Install-Prerequisites.ps1`, and `Prerequisites.cmd`. Keep them together in the
package directory. Target-side setup needs no SDK, Python, or network access.

## Supported systems and inventory

Use native x64 Windows PowerShell 5.1 on Windows 11 24H2 or Windows Server 2025
x64. The read-only check inventories the x64 System32 CRT DLLs against the
bundled VC floor and the highest healthy current-user x64 framework in the
governed Microsoft runtime family at or above the minimum. Another user's
registration does not count; newer serviced versions do. Ready components skip
without requiring their installer files. Every installer still needed is
hash/signature-verified before installation, then exit codes and actual
postconditions are checked.

A ready inventory is not a bootstrap or application-launch test. Validate the
actual application separately. Installation uses the complete governed runtime
installer when needed; do not extract and install a framework MSIX alone or infer
package requirements from another SDK release or app's features.

## Check or install as the application user

Open a native x64 Windows PowerShell 5.1 window and change to the directory
containing these six files. Use the original application account with a
non-administrator token:

```powershell
# Read-only check.
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File .\Install-Prerequisites.ps1

# Explicit installation; Prerequisites.cmd performs the same operation.
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File .\Install-Prerequisites.ps1 -Install
```

Only the signed VC redistributable requests normal UAC elevation; Windows App
Runtime registration stays with the original user. Any administrator token
refuses, including the built-in Administrator or accounts without a UAC split
token when UAC is disabled. Use an ordinary application account in those
environments. Declining UAC stops the flow and the runtime installer is not
attempted. The helper never handles administrator credentials.

An elevated administrator may prepare only the machine CRT:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File .\Install-Prerequisites.ps1 -InstallVcRuntime
```

This mode never queries or registers the Windows App Runtime. Afterwards run
`-Install` as the ordinary application user; it skips the ready CRT and prepares
that user's runtime. Never substitute an elevated arbitrary-script broker.

## Results and safe retry

Once the script starts, its JSON report records the actual caller, mode,
component state, actions and error. PowerShell startup, parsing or
execution-policy failures can occur before any report exists; retain their error
output. Exit codes: `0` selected mode ready, `3` missing in a read-only check,
`1` error, `1223` UAC declined, `3010` CRT restart required. Restart manually and
rerun when required. The helper does not download prerequisites or automatically
reboot, downgrade, unregister, force-stop, or roll back. Retain failure reports
and diagnose partial state.

The installer checks use Windows Authenticode trust and an exact Microsoft
publisher CN; they do not pin a particular root certificate.

Unsigned Internet-marked or UNC scripts can be blocked by `RemoteSigned`,
including scripts extracted from a downloaded ZIP. Before using this package,
verify the hash of the entire package against an authenticated publisher/source
record obtained independently of the package. A manifest beside the script is
not independent proof of the script's authenticity. After verification, extract
or copy the package to a local directory. If `RemoteSigned` blocks the reviewed
helper, from that package directory unblock only that helper:

```powershell
Unblock-File -LiteralPath '.\Install-Prerequisites.ps1'
```

Then rerun `Prerequisites.cmd` or the PowerShell command from the same package
directory. Do not recursively unblock unrelated files or lower persistent
machine/group execution policy.
