# Stage an offline prerequisite bundle; this never installs host dependencies.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$VcRedistPath,
    [Parameter(Mandatory=$true)][string]$VcToolsVersion,
    [Parameter(Mandatory=$true)][string]$WindowsAppRuntimeInstallerPath,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$contract = Get-Content -LiteralPath (Join-Path $root 'dependency-contract.json') -Raw | ConvertFrom-Json
$runtime = $contract.dependencies.'windows-app-sdk'.runtime
if ($runtime.architecture -ne 'x64') {throw 'Only the governed x64 deployment is supported'}
$toolset = $null
if (-not [Version]::TryParse($VcToolsVersion, [ref]$toolset) -or $toolset.Major -ne 14 -or $toolset.Build -lt 0) {throw 'Expected the consumed MSVC v14 toolset version, including its build component'}
# Resolve against PowerShell's location, not the unrelated .NET working directory.
$VcRedistPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VcRedistPath)
$WindowsAppRuntimeInstallerPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WindowsAppRuntimeInstallerPath)
$destinationPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
if (Test-Path -LiteralPath $destinationPath) {throw 'Destination exists; refusing replacement'}

function Assert-MicrosoftInstaller([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {throw "Installer missing: $Path"}
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate -or
        -not [regex]::IsMatch($signature.SignerCertificate.Subject, '\ACN=Microsoft Corporation(?:,|\z)')) {
        throw "Installer is not trusted Microsoft Authenticode: $Path"
    }
}
Assert-MicrosoftInstaller $VcRedistPath
Assert-MicrosoftInstaller $WindowsAppRuntimeInstallerPath
$runtimeHash = (Get-FileHash -LiteralPath $WindowsAppRuntimeInstallerPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($runtimeHash -ne $runtime.sha256) {throw 'Windows App Runtime installer differs from the governed SHA-256'}
$vcFile = Get-Item -LiteralPath $VcRedistPath
$v = $vcFile.VersionInfo
$vcVersion = New-Object Version($v.FileMajorPart,$v.FileMinorPart,$v.FileBuildPart,$v.FilePrivatePart)
$minimumToolset = New-Object Version($toolset.Major,$toolset.Minor,$toolset.Build,[Math]::Max($toolset.Revision,0))
if ($v.OriginalFilename -ine 'vc_redist.x64.exe') {throw 'Expected the x64 Visual C++ redistributable, not another signed executable'}
if ($vcVersion.Major -ne $minimumToolset.Major -or $vcVersion -lt $minimumToolset) {throw 'VC redistributable predates the consumed MSVC toolset version'}
$helperRoot = Join-Path $root 'app\windows-winui\deployment'
$helperNames = @('Install-Prerequisites.ps1','Prerequisites.cmd','README.md')
foreach ($name in $helperNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $helperRoot $name) -PathType Leaf)) {throw "Deployment helper missing: $name"}
}
$manifest = [ordered]@{
    schemaVersion = 1
    vcToolsVersion = $VcToolsVersion
    vcRuntime = [ordered]@{
        version = $vcVersion.ToString(4)
        sha256 = (Get-FileHash -LiteralPath $VcRedistPath -Algorithm SHA256).Hash.ToLowerInvariant()
        installer = 'vc_redist.x64.exe'
    }
    windowsAppRuntime = [ordered]@{
        packageName = $runtime.packageName
        packageFamilyName = $runtime.packageName + '_8wekyb3d8bbwe'
        version = $runtime.version
        architecture = $runtime.architecture.ToUpperInvariant()
        sha256 = $runtimeHash
        installer = 'WindowsAppRuntimeInstall-x64.exe'
    }
}
# All inputs are validated before creating output. I/O failures retain partial
# output for diagnosis; a repeated stage refuses rather than silently replacing it.
[IO.Directory]::CreateDirectory($destinationPath) | Out-Null
[IO.File]::Copy($VcRedistPath, (Join-Path $destinationPath 'vc_redist.x64.exe'))
[IO.File]::Copy($WindowsAppRuntimeInstallerPath, (Join-Path $destinationPath 'WindowsAppRuntimeInstall-x64.exe'))
foreach ($name in $helperNames) {
    $source = Join-Path $helperRoot $name
    $target = Join-Path $destinationPath $name
    [IO.File]::Copy($source, $target)
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) {throw 'Copied helper hash mismatch'}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destinationPath 'prerequisites.json') -Encoding Ascii
foreach ($name in @('vc_redist.x64.exe','WindowsAppRuntimeInstall-x64.exe')) {
    $expected = if ($name -eq 'vc_redist.x64.exe') {$manifest.vcRuntime.sha256} else {$manifest.windowsAppRuntime.sha256}
    if ((Get-FileHash -LiteralPath (Join-Path $destinationPath $name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {throw 'Copied installer hash mismatch'}
}
Write-Output "Staged offline prerequisites: $destinationPath"
