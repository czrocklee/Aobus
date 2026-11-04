# Non-advanced, parameterless -File entry: the leading marker protects portal
# options from script binding; every remaining argument is literal application data.
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
if ($args.Count -lt 1 -or $args[0] -ne "--") {
    throw "The Aobus bootstrap requires its portal argument marker."
}
[string[]]$portalArguments = @()
if ($args.Count -gt 1) {
    $portalArguments = $args[1..($args.Count - 1)]
}
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$StateRoot = [IO.Path]::GetFullPath($env:AOBUS_STATE_ROOT)
$VcpkgRoot = $env:VCPKG_ROOT
$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot "toolchain.json") -Raw | ConvertFrom-Json
if ([int]$config.schemaVersion -ne 1) {
    throw "Unsupported Aobus toolchain schema: $($config.schemaVersion)"
}
$pythonVersion = [string]$config.python
$pythonRoot = Join-Path $StateRoot "tools\python"
$finalRoot = Join-Path $pythonRoot $pythonVersion
$python = Join-Path $finalRoot "python.exe"
$probe = "import ensurepip, sys, venv; raise SystemExit(0 if sys.version_info[:3] == ($($pythonVersion.Replace('.', ', '))) else 1)"

function Test-AobusPython([string]$Candidate) {
    if (-not (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        return $false
    }
    & $Candidate -I -c $probe *> $null
    return $LASTEXITCODE -eq 0
}

function Initialize-AobusPython {
    if (Test-AobusPython $python) {
        return
    }
    $stateBytes = [Text.Encoding]::UTF8.GetBytes([IO.Path]::GetFullPath($StateRoot).ToLowerInvariant())
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $stateHash = ([BitConverter]::ToString($sha256.ComputeHash($stateBytes))).Replace("-", "")
    } finally {
        $sha256.Dispose()
    }
    $mutex = New-Object Threading.Mutex($false, "Local\AobusPython-$stateHash")
    $acquired = $false
    try {
        try {
            $acquired = $mutex.WaitOne([TimeSpan]::FromMinutes(10))
        } catch [Threading.AbandonedMutexException] {
            $acquired = $true
        }
        if (-not $acquired) {
            throw "Timed out waiting for another Aobus Python bootstrap."
        }
        if (Test-AobusPython $python) {
            return
        }
        if (-not $VcpkgRoot) {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
            if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
                throw "Visual Studio Build Tools with the C++ workload are required to bootstrap Aobus Python."
            }
            $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -ne 0 -or -not $visualStudio) {
                throw "Could not find Visual Studio Build Tools with the C++ x64 toolset."
            }
            $VcpkgRoot = Join-Path ([string]($visualStudio | Select-Object -Last 1)) "VC\vcpkg"
        }
        $vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
        if (-not (Test-Path -LiteralPath $vcpkg -PathType Leaf)) {
            throw "Could not find vcpkg.exe under VCPKG_ROOT: $VcpkgRoot"
        }
        $nugetLines = @(& $vcpkg fetch nuget)
        if ($LASTEXITCODE -ne 0) {
            throw "vcpkg failed to provision NuGet."
        }
        $nuget = $nugetLines | ForEach-Object { ([string]$_).Trim() } |
            Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
            Select-Object -Last 1
        if (-not $nuget) {
            throw "vcpkg did not report a usable NuGet executable."
        }
        New-Item -ItemType Directory -Force -Path $pythonRoot | Out-Null
        $token = [Guid]::NewGuid().ToString("N")
        $staging = Join-Path $pythonRoot ".staging-$token"
        $backup = Join-Path $pythonRoot ".previous-$token"
        try {
            New-Item -ItemType Directory -Path $staging | Out-Null
            & $nuget install python -Version $pythonVersion -OutputDirectory $staging -DirectDownload -NonInteractive `
                -Source "https://api.nuget.org/v3/index.json" -Verbosity quiet
            if ($LASTEXITCODE -ne 0) {
                throw "NuGet failed to install Python $pythonVersion."
            }
            $installed = Join-Path $staging "python.$pythonVersion\tools"
            $installedPython = Join-Path $installed "python.exe"
            if (-not (Test-AobusPython $installedPython)) {
                throw "The downloaded Python $pythonVersion package failed its runtime probe."
            }
            if (Test-Path -LiteralPath $finalRoot) {
                Move-Item -LiteralPath $finalRoot -Destination $backup
            }
            try {
                Move-Item -LiteralPath $installed -Destination $finalRoot
            } catch {
                if ((Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $finalRoot)) {
                    Move-Item -LiteralPath $backup -Destination $finalRoot
                }
                throw
            }
            if (-not (Test-AobusPython $python)) {
                throw "The installed Aobus Python $pythonVersion failed its runtime probe."
            }
            if (Test-Path -LiteralPath $backup) {
                Remove-Item -LiteralPath $backup -Recurse -Force
            }
        } finally {
            if (Test-Path -LiteralPath $staging) {
                Remove-Item -LiteralPath $staging -Recurse -Force
            }
        }
    } finally {
        if ($acquired) {
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
    }
}

function ConvertTo-NativeArgument([string]$Value) {
    # CRT argv: 2n+1 backslashes before a literal quote, 2n at the closing quote.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

Initialize-AobusPython
# Do not pass PowerShell host mutations to the portal. The batch caller records
# these before entering PowerShell; no other caller environment is reconstructed.
$env:PSModulePath = $env:_AOBUS_ORIGINAL_PS_MODULE_PATH
$env:PSExecutionPolicyPreference = $env:_AOBUS_ORIGINAL_PS_EXECUTION_POLICY
$env:_AOBUS_ORIGINAL_PS_MODULE_PATH = $null
$env:_AOBUS_ORIGINAL_PS_EXECUTION_POLICY = $null
$env:PYTHONPATH = Join-Path $projectRoot "script"
$launchArguments = @("-m", "ao.core.pythonenv", "--project-root", $projectRoot,
    "--state-root", $StateRoot, "--launch-windows-portal", "--") + $portalArguments
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $python
$start.Arguments = ($launchArguments | ForEach-Object { ConvertTo-NativeArgument ([string]$_) }) -join " "
$start.UseShellExecute = $false
$child = New-Object Diagnostics.Process
try {
    $child.StartInfo = $start
    if (-not $child.Start()) {
        throw "Could not start the Aobus Python portal."
    }
    $child.WaitForExit()
    $status = $child.ExitCode
} finally {
    $child.Dispose()
}
exit $status
