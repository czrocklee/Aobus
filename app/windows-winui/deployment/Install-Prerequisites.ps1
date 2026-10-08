<#
.SYNOPSIS
  Prerequisite check/bootstrap for the Aobus WinUI validation package: the
  MSVC VC++ CRT (machine redist) and the Windows App Runtime (per-user).
  Windows PowerShell 5.1, 64-bit process. No Python/SDK/download/source shares.

.DESCRIPTION
  (default)  Read-only check of the CURRENT user context (never -AllUsers).
  -Install   Standard-user bootstrap; refuses any administrator token,
    including accounts without a UAC split token. Use the original ordinary
    application account. Elevates only the fixed VC redist EXE via native UAC
    (-Verb RunAs; no credentials handled here), then runs the runtime
    installer as the ORIGINAL user without elevation: --quiet.
  -InstallVcRuntime  Admin machine-only step for unattended provisioning over
    admin SSH. Full admin token required; installs ONLY the VC CRT
    (/install /quiet /norestart); NEVER installs/registers the Windows App
    Runtime.
  prerequisites.json (release staging, next to this script) holds the minimum
  versions (dependency contract; not hardcoded) and installer SHA-256 values;
  installers live in the manifest directory under fixed basenames. Before any
  mutation, every installer needed by the selected mode must match SHA-256 and
  carry a valid Authenticode signature anchored to 'CN=Microsoft Corporation';
  missing/tampered input fails hard before the first mutation. Healthy
  dependencies are skipped, never downgraded/unregistered/rebooted;
  postconditions are re-detected, not assumed from exit codes; 3010 reports a
  required reboot and never reboots. If all is ready, -Install is a no-op
  needing no installer files. Output: one structured JSON report on stdout
  (no secrets). Exit codes: 0 ready, 3 missing, 1 error, 1223 UAC declined,
  3010 reboot required.
#>
[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Install')]
    [switch]$Install,
    [Parameter(ParameterSetName = 'MachineVc')]
    [switch]$InstallVcRuntime,
    [string]$ManifestPath   # Defaults to prerequisites.json next to this script.
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

# ------------------------------------------------------------------ constants
$ExpectedSchemaVersion    = 1
$ExpectedVcInstaller      = 'vc_redist.x64.exe'
$ExpectedRuntimeInstaller = 'WindowsAppRuntimeInstall-x64.exe'
$ExpectedRuntimeName      = 'Microsoft.WindowsAppRuntime.2'
$ExpectedRuntimeFamily    = 'Microsoft.WindowsAppRuntime.2_8wekyb3d8bbwe'
$ExpectedArchitecture     = 'X64'
$MicrosoftSignerPattern   = '\ACN=Microsoft Corporation(?:,|\z)'
$VcRuntimeDlls = @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll')

# --------------------------------------------------------- pure helper seams
function ConvertTo-VersionTuple {
    # Strict ASCII four-part dotted version, each part <= 65535 (the AppX
    # Runtime PackageVersion contract). No trimming, no prefix/suffix
    # tolerance; anything else returns $null, never throws.
    param([string]$VersionString)
    if (-not $VersionString -or $VersionString -notmatch '\A[0-9]{1,5}(?:\.[0-9]{1,5}){3}\z') { return $null }
    $tuple = @()
    foreach ($part in ($VersionString -split '\.')) {
        $value = 0L
        if (-not [int64]::TryParse($part, [ref]$value) -or $value -gt 65535) { return $null }
        $tuple += $value
    }
    return $tuple
}

function Test-VersionAtLeast {
    param([string]$Installed, [string]$Minimum)
    $installedTuple = ConvertTo-VersionTuple -VersionString $Installed
    $minimumTuple = ConvertTo-VersionTuple -VersionString $Minimum
    if (-not $installedTuple -or -not $minimumTuple) { return $false }
    for ($i = 0; $i -lt 4; $i++) {
        if ($installedTuple[$i] -gt $minimumTuple[$i]) { return $true }
        if ($installedTuple[$i] -lt $minimumTuple[$i]) { return $false }
    }
    return $true
}

function Test-ManifestShape {
    # Schema 1: fixed installer basenames and package identity; versions are
    # 4-part numeric, hashes 64 lowercase hex. A missing/bad object fails
    # closed to $false (property access under StrictMode throws), never leaks.
    param($Manifest)
    try {
        if (-not $Manifest -or "$($Manifest.schemaVersion)" -ne "$ExpectedSchemaVersion") { return $false }
        $vc = $Manifest.vcRuntime
        if (-not $vc -or $vc.installer -ne $ExpectedVcInstaller) { return $false }
        if (-not (ConvertTo-VersionTuple -VersionString $vc.version)) { return $false }
        if ($vc.sha256 -cnotmatch '\A[0-9a-f]{64}\z') { return $false }
        $runtime = $Manifest.windowsAppRuntime
        if (-not $runtime -or $runtime.installer -ne $ExpectedRuntimeInstaller) { return $false }
        if ($runtime.packageName -ne $ExpectedRuntimeName -or $runtime.packageFamilyName -ne $ExpectedRuntimeFamily) { return $false }
        if ($runtime.architecture -ne $ExpectedArchitecture) { return $false }
        if (-not (ConvertTo-VersionTuple -VersionString $runtime.version)) { return $false }
        if ($runtime.sha256 -cnotmatch '\A[0-9a-f]{64}\z') { return $false }
        return $true
    } catch { return $false }
}

# ---------------------------------------------------------- detection seams
function Get-VcRuntimeState {
    # 64-bit CRT DLLs in System32; all must exist with FileVersion >= minimum.
    param([string]$RequiredVersion)
    $versions = [ordered]@{}
    $missing = @()
    foreach ($dll in $VcRuntimeDlls) {
        $path = Join-Path $env:SystemRoot ('System32\' + $dll)
        $version = $null
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            # FileVersion may carry trailing product text; build the 4-tuple
            # from the explicit Part properties instead of loose parsing.
            $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
            $version = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
        }
        $versions[$dll] = $version
        if (-not (Test-VersionAtLeast -Installed $version -Minimum $RequiredVersion)) { $missing += $dll }
    }
    return [ordered]@{ ready = ($missing.Count -eq 0); requiredVersion = $RequiredVersion;
        versions = $versions; missing = $missing }
}

function Get-AppRuntimeState {
    # Inventory framework registration for this user, not bootstrap execution.
    # An AllUsers registration does not satisfy a user without a per-user one.
    param([string]$RequiredVersion)
    $selectedVersion = $null
    foreach ($package in @(Get-AppxPackage -Name $ExpectedRuntimeName -PackageTypeFilter Framework)) {
        try {
            # Wrong name/status/family/architecture/IsFramework or a malformed
            # version is ignored, never thrown; highest valid version wins.
            if ($package.Name -cne $ExpectedRuntimeName -or $package.PackageFamilyName -cne $ExpectedRuntimeFamily) { continue }
            if ("$($package.Status)" -ne 'Ok' -or "$($package.Architecture)" -ne $ExpectedArchitecture) { continue }
            if ($package.PSObject.Properties['IsFramework'] -and $package.IsFramework -ne $true) { continue }
            $versionText = "$($package.Version)"
            if (-not (Test-VersionAtLeast -Installed $versionText -Minimum $RequiredVersion)) { continue }
            $version = [version]$versionText
            if (-not $selectedVersion -or $version -gt $selectedVersion) { $selectedVersion = $version }
        } catch { continue }
    }
    return [ordered]@{ ready = ($null -ne $selectedVersion); requiredVersion = $RequiredVersion;
        selectedVersion = $(if ($selectedVersion) { "$($selectedVersion)" } else { $null }) }
}

function Test-InstallerIntegrity {
    # SHA-256 must match the trusted manifest. Authenticode must validate
    # under Windows trust and the subject must begin with the exact Microsoft
    # CN component; this is a publisher check, not a pinned root certificate.
    param([string]$Path, [string]$ExpectedSha256)
    $name = Split-Path -Leaf $Path
    $deny = { param([string]$Detail) [pscustomobject]@{ Ok = $false; Detail = $Detail } }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return (& $deny "missing installer: $name") }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $ExpectedSha256) { return (& $deny "SHA-256 mismatch: $name") }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
        return (& $deny ("Authenticode not valid ({0}): {1}" -f $signature.Status, $name))
    }
    if (-not [regex]::IsMatch($signature.SignerCertificate.Subject, $MicrosoftSignerPattern)) {
        return (& $deny "unexpected signer: $name")
    }
    return [pscustomobject]@{ Ok = $true; Detail = "verified: $name" }
}

function Test-ElevatedAdministrator {
    # Check enabled administrator membership in the actual current token,
    # including full tokens from accounts without UAC split tokens.
    # No parameter can fake a role here. Native fixtures may AST-import the
    # original functions and insert a test-only override of this one seam
    # before the main block; production always reads the real token.
    $principal = New-Object System.Security.Principal.WindowsPrincipal([System.Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole((New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')))
}

# ----------------------------------------------------------------- report/main
$Report = [ordered]@{
    # Actual authenticated identity; environment variables are spoofable and
    # must not be used to state the account.
    user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    mode = $PSCmdlet.ParameterSetName.ToLowerInvariant()
    vcRuntime = $null; windowsAppRuntime = $null; actions = @()
    rebootRequired = $false; exitCode = 0; error = $null
}
function Add-Action { param([string]$Text) $Report.actions += $Text }
function Finish {
    # Write the JSON report explicitly to stdout (never into the return
    # pipeline) so callers receive only the numeric exit code.
    param([int]$Code)
    $Report.exitCode = $Code
    [Console]::Out.WriteLine(($Report | ConvertTo-Json -Depth 6))
    return $Code
}
function Fail { param([string]$Message) throw $Message }

try {
    # A host without a console may reject the encoding setter. Keep reporting
    # through its existing stdout writer instead of failing before the handler.
    try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false) }
    catch { Add-Action 'stdout-encoding-default' }
    if (($PSCmdlet.ParameterSetName -eq 'Install' -and -not $Install) -or
        ($PSCmdlet.ParameterSetName -eq 'MachineVc' -and -not $InstallVcRuntime)) {
        Fail 'An install mode requires an explicit true switch; omit it for a read-only check.'
    }
    if (-not [Environment]::Is64BitProcess) { Fail 'Run the 64-bit Windows PowerShell; this helper is x64 only.' }
    # Explicit native x64: an ARM64 host (or emulated x64) would expose ARM64
    # binaries in System32 and invalidate the CRT check.
    if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { Fail 'Native x64 Windows (PROCESSOR_ARCHITECTURE=AMD64) is required.' }
    if ($env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { Fail 'ARM64-emulated x64 is not supported; System32 would contain ARM64 binaries.' }
    if (-not $ManifestPath) { $ManifestPath = Join-Path $PSScriptRoot 'prerequisites.json' }
    $ManifestPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ManifestPath)
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { Fail "prerequisites manifest not found: '$ManifestPath'" }
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if (-not (Test-ManifestShape -Manifest $Manifest)) { Fail "prerequisites manifest does not match schema version ${ExpectedSchemaVersion}: '$ManifestPath'" }
    Add-Action 'manifest-validated'
    $InstallerDirectory = Split-Path -Parent $ManifestPath
    $VcInstallerPath = Join-Path $InstallerDirectory $Manifest.vcRuntime.installer
    $RuntimeInstallerPath = Join-Path $InstallerDirectory $Manifest.windowsAppRuntime.installer
    $VcFloor = $Manifest.vcRuntime.version
    $RuntimeFloor = $Manifest.windowsAppRuntime.version

    $vcState = Get-VcRuntimeState -RequiredVersion $VcFloor
    $runtimeState = $null
    if ($PSCmdlet.ParameterSetName -ne 'MachineVc') {
        $runtimeState = Get-AppRuntimeState -RequiredVersion $RuntimeFloor
    }
    $Report.vcRuntime = $vcState
    $Report.windowsAppRuntime = $runtimeState
    Add-Action 'detected-current-user-state'
    $isAdmin = Test-ElevatedAdministrator

    switch ($PSCmdlet.ParameterSetName) {
        'Check' {
            $code = 0
            if (-not $vcState.ready -or -not $runtimeState.ready) { $code = 3 }
            exit (Finish $code)
        }
        'Install' {
            if ($isAdmin) { Fail 'Run -Install as the original application user with a non-administrator token. Administrator accounts without a UAC split token are not supported by this mode; use an ordinary application account. -InstallVcRuntime prepares only the machine CRT.' }
            $vcNeeded = -not $vcState.ready
            $runtimeNeeded = -not $runtimeState.ready
            if (-not $vcNeeded -and -not $runtimeNeeded) { Add-Action 'ready-noop'; exit (Finish 0) }

            # Validate EVERY installer this mode still needs BEFORE any mutation.
            foreach ($needed in @(
                @{ Selected = $vcNeeded; Path = $VcInstallerPath; Sha256 = $Manifest.vcRuntime.sha256 },
                @{ Selected = $runtimeNeeded; Path = $RuntimeInstallerPath; Sha256 = $Manifest.windowsAppRuntime.sha256 })) {
                if (-not $needed.Selected) { continue }
                $check = Test-InstallerIntegrity -Path $needed.Path -ExpectedSha256 $needed.Sha256
                Add-Action $check.Detail
                if (-not $check.Ok) { Fail ("prerequisite installer failed verification: {0}; refusing before any change" -f $check.Detail) }
            }

            if ($vcNeeded) {
                # Fixed signed redist EXE only; native UAC elevation; the
                # script itself never handles credentials. A declined UAC
                # prompt surfaces as Win32Exception 1223 from Start-Process.
                $proc = $null
                try {
                    $proc = Start-Process -FilePath $VcInstallerPath -ArgumentList @('/install', '/quiet', '/norestart') -Verb RunAs -Wait -PassThru
                } catch {
                    $base = $_.Exception.GetBaseException()
                    if ($base -is [System.ComponentModel.Win32Exception] -and $base.NativeErrorCode -eq 1223) {
                        $Report.error = 'UAC elevation was declined (1223); no runtime install was attempted.'
                        exit (Finish 1223)
                    }
                    throw
                }
                $vcExit = $proc.ExitCode
                Add-Action ("vc-redist-exit={0}" -f $vcExit)
                if ($vcExit -eq 1223) {
                    # Defensive: a declined elevation normally throws instead.
                    $Report.error = 'UAC elevation was declined (1223); no runtime install was attempted.'
                    exit (Finish 1223)
                }
                # Only 0/1638/3010 are accepted; any other exit fails even if
                # the DLLs currently look ready.
                if ($vcExit -ne 0 -and $vcExit -ne 1638 -and $vcExit -ne 3010) {
                    Fail ("VC redist returned unexpected exit code {0}; refusing to continue" -f $vcExit)
                }
                $vcState = Get-VcRuntimeState -RequiredVersion $VcFloor
                $Report.vcRuntime = $vcState
                if ($vcExit -eq 1638) { Add-Action 'vc-redist-1638-already-installed' }
                if (($vcExit -eq 0 -or $vcExit -eq 1638) -and -not $vcState.ready) {
                    Fail ("VC CRT postcondition not satisfied after redist exit {0}" -f $vcExit)
                }
                if ($vcExit -eq 3010) {
                    # Reboot pending (file rename); stop after the CRT step
                    # even if the postcondition is not yet satisfied.
                    $Report.rebootRequired = $true
                    Add-Action 'vc-redist-reboot-required-stop'
                    exit (Finish 3010)
                }
            } else { Add-Action 'vc-skip-ready' }

            if ($runtimeNeeded) {
                # Original (non-elevated) user; per-user registration, no -Verb
                # RunAs. Contract: exit 0 AND a healthy current-user
                # postcondition; any other exit fails even if the package
                # appears registered.
                $proc = Start-Process -FilePath $RuntimeInstallerPath -ArgumentList @('--quiet') -Wait -PassThru
                Add-Action ("runtime-installer-exit={0}" -f $proc.ExitCode)
                $runtimeState = Get-AppRuntimeState -RequiredVersion $RuntimeFloor
                $Report.windowsAppRuntime = $runtimeState
                if ($proc.ExitCode -ne 0 -or -not $runtimeState.ready) {
                    Fail ("Windows App Runtime postcondition not satisfied (installer exit {0}, current-user ready: {1})" -f $proc.ExitCode, $runtimeState.ready)
                }
            } else { Add-Action 'runtime-skip-ready' }
            exit (Finish 0)
        }
        'MachineVc' {
            if (-not $isAdmin) { Fail 'InstallVcRuntime requires a full administrator token (unattended admin SSH provisioning); run it from an elevated shell.' }
            if ($vcState.ready) { Add-Action 'vc-skip-ready-noop'; exit (Finish 0) }
            $check = Test-InstallerIntegrity -Path $VcInstallerPath -ExpectedSha256 $Manifest.vcRuntime.sha256
            Add-Action $check.Detail
            if (-not $check.Ok) { Fail ("prerequisite installer failed verification: {0}; refusing before any change" -f $check.Detail) }
            # Machine-only mode: VC CRT only. The Windows App Runtime is NEVER
            # installed or registered here.
            $proc = Start-Process -FilePath $VcInstallerPath -ArgumentList @('/install', '/quiet', '/norestart') -Wait -PassThru
            $vcExit = $proc.ExitCode
            Add-Action ("vc-redist-exit={0}" -f $vcExit)
            # Only 0/1638/3010 are accepted; any other exit fails even if the
            # DLLs currently look ready.
            if ($vcExit -ne 0 -and $vcExit -ne 1638 -and $vcExit -ne 3010) {
                Fail ("VC redist returned unexpected exit code {0}; refusing to continue" -f $vcExit)
            }
            $vcState = Get-VcRuntimeState -RequiredVersion $VcFloor
            $Report.vcRuntime = $vcState
            if ($vcExit -eq 1638) { Add-Action 'vc-redist-1638-already-installed' }
            if (($vcExit -eq 0 -or $vcExit -eq 1638) -and -not $vcState.ready) {
                Fail ("VC CRT postcondition not satisfied after redist exit {0}" -f $vcExit)
            }
            if ($vcExit -eq 3010) { $Report.rebootRequired = $true; exit (Finish 3010) }
            exit (Finish 0)
        }
    }
    Fail "unhandled parameter set '$($PSCmdlet.ParameterSetName)'"
}
catch {
    $Report.error = $_.Exception.Message
    exit (Finish 1)
}
