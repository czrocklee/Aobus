# Native test-only driver. No injection API ships in production.
# Scenario mode mocks every installer spawn; pure mode evaluates the original
# helper functions; stage mode runs the complete stager with mocked signature
# and EXE metadata but real files, hashes and manifests. Parse mode checks syntax.
# Scenario plans are strict and finite. A missing, empty, or exhausted plan
# records fixture-violation and throws; trailing unused plans are allowed.
# Stage mode records the same violation if Start-Process is reached.
# No real process is ever started.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProductionSource,
    [Parameter(Mandatory = $true)][string]$ScenarioFile,
    [Parameter(Mandatory = $true)][string]$WorkDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
# A pwsh parent can leave PS 5.1 with a foreign module search path. Load the
# native Utility module explicitly so its Get-FileHash function stays real.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$scenario = Get-Content -LiteralPath $ScenarioFile -Raw | ConvertFrom-Json
foreach ($name in @('manifest','mock','cases','createInstallers','fixtureFile','sources','switches','stage','relativeManifest')) {
    if (-not $scenario.PSObject.Properties[$name]) {
        $scenario | Add-Member -MemberType NoteProperty -Name $name -Value $null
    }
}

if ($scenario.kind -eq 'parse') {
    $sources = @($ProductionSource) + @($scenario.sources)
    foreach ($path in $sources) {
        if (-not $path) { continue }
        $tokens = $null; $parseErrors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors)
        if ($parseErrors.Count -gt 0) { throw ('parse failure in ' + $path + ': ' + $parseErrors[0].Message) }
    }
    [pscustomobject]@{ parsed = @($sources) } | ConvertTo-Json -Compress
    exit 0
}

$source = [IO.File]::ReadAllText($ProductionSource)
[IO.Directory]::CreateDirectory($WorkDirectory) | Out-Null

if ($scenario.kind -eq 'stage') {
    # Only the test mirror's runtime hash changes to bind synthetic bytes.
    # The selected checkout and real installers are never modified or executed.
    $repo = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $ProductionSource) '..\..'))
    $mirror = Join-Path $WorkDirectory 'Aobus [offline]'
    $scriptDirectory = Join-Path $mirror 'script\windows'
    $helperDirectory = Join-Path $mirror 'app\windows-winui\deployment'
    [IO.Directory]::CreateDirectory($scriptDirectory) | Out-Null
    [IO.Directory]::CreateDirectory($helperDirectory) | Out-Null
    $stager = Join-Path $scriptDirectory 'stage-prerequisites.ps1'
    [IO.File]::WriteAllText($stager, $source)
    foreach ($name in @('Install-Prerequisites.ps1','Prerequisites.cmd','README.md')) {
        # Omit only from the isolated mirror, never from the real checkout.
        if ($name -eq 'README.md' -and $scenario.stage.missingReadme) { continue }
        [IO.File]::Copy((Join-Path $repo ('app\windows-winui\deployment\' + $name)), (Join-Path $helperDirectory $name))
    }
    $vcInput = Join-Path $WorkDirectory 'vc [fixture].exe'
    $runtimeInput = Join-Path $WorkDirectory 'runtime [fixture].exe'
    [IO.File]::WriteAllText($vcInput, 'synthetic VC installer')
    [IO.File]::WriteAllText($runtimeInput, 'synthetic runtime installer')
    $contract = Get-Content -LiteralPath (Join-Path $repo 'dependency-contract.json') -Raw | ConvertFrom-Json
    $contract.dependencies.'windows-app-sdk'.runtime.sha256 = (Get-FileHash -LiteralPath $runtimeInput -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($scenario.stage.badRuntimeHash) { $contract.dependencies.'windows-app-sdk'.runtime.sha256 = '0' * 64 }
    $contract | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $mirror 'dependency-contract.json') -Encoding Ascii
    $destination = Join-Path $WorkDirectory 'Prerequisites [output]'
    if ($scenario.stage.destinationExists) {
        [IO.Directory]::CreateDirectory($destination) | Out-Null
        [IO.File]::WriteAllText((Join-Path $destination 'sentinel'), 'preserve me')
    }
    # Global here means this isolated test process, not the machine/user profile.
    # Invoking a child script changes $script: resolution inside inherited mocks.
    $global:StagePlan = $scenario.stage
    $global:StageCalls = @()
    function Get-AuthenticodeSignature {
        param([string]$LiteralPath)
        $global:StageCalls += [pscustomobject]@{ kind='signature'; path=$LiteralPath }
        $certificate = $null
        if ($global:StagePlan.subject) { $certificate = [pscustomobject]@{ Subject=$global:StagePlan.subject } }
        return [pscustomobject]@{ Status=$global:StagePlan.status; SignerCertificate=$certificate }
    }
    function Get-Item {
        param([string]$LiteralPath)
        $global:StageCalls += [pscustomobject]@{ kind='version'; path=$LiteralPath }
        return [pscustomobject]@{ VersionInfo=$global:StagePlan.versionInfo }
    }
    function Start-Process {
        param([string]$FilePath, [string[]]$ArgumentList, [string]$Verb, [switch]$Wait, [switch]$PassThru)
        # Poison the call log before throwing so a caught error cannot look valid.
        $global:StageCalls += [pscustomobject]@{ kind='fixture-violation'; path=$FilePath }
        throw 'fixture violation: staging must never invoke an installer'
    }
    $stageError = $null; $messages = @()
    $vcArgument = $vcInput; $runtimeArgument = $runtimeInput; $destinationArgument = $destination
    $otherDirectory = Split-Path -Parent $WorkDirectory
    $sourceHashes = [ordered]@{
        'vc_redist.x64.exe' = (Get-FileHash -LiteralPath $vcInput -Algorithm SHA256).Hash.ToLowerInvariant()
        'WindowsAppRuntimeInstall-x64.exe' = (Get-FileHash -LiteralPath $runtimeInput -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($scenario.stage.relativePaths) {
        # Same basenames, wrong bytes: resolution must use PowerShell's location.
        [IO.File]::WriteAllText((Join-Path $otherDirectory (Split-Path -Leaf $vcInput)), 'wrong VC bytes')
        [IO.File]::WriteAllText((Join-Path $otherDirectory (Split-Path -Leaf $runtimeInput)), 'wrong runtime bytes')
        Set-Location -LiteralPath $WorkDirectory
        [Environment]::CurrentDirectory = $otherDirectory
        $vcArgument = Split-Path -Leaf $vcInput
        $runtimeArgument = Split-Path -Leaf $runtimeInput
        $destinationArgument = Split-Path -Leaf $destination
    }
    try {
        $messages = @(& $stager -VcRedistPath $vcArgument -VcToolsVersion $scenario.stage.toolset -WindowsAppRuntimeInstallerPath $runtimeArgument -Destination $destinationArgument)
    } catch { $stageError = $_.Exception.Message }
    $manifest = $null; $hashes = [ordered]@{}
    if (Test-Path -LiteralPath (Join-Path $destination 'prerequisites.json')) {
        $manifest = Get-Content -LiteralPath (Join-Path $destination 'prerequisites.json') -Raw | ConvertFrom-Json
        foreach ($name in @('vc_redist.x64.exe','WindowsAppRuntimeInstall-x64.exe','Install-Prerequisites.ps1','Prerequisites.cmd','README.md')) {
            $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $sentinel = $null
    if (Test-Path -LiteralPath (Join-Path $destination 'sentinel')) { $sentinel = [IO.File]::ReadAllText((Join-Path $destination 'sentinel')) }
    [pscustomobject]@{ succeeded=($null -eq $stageError); error=$stageError; messages=$messages; manifest=$manifest; hashes=$hashes; sourceHashes=$sourceHashes; calls=$global:StageCalls; destinationExists=(Test-Path -LiteralPath $destination); otherDestinationExists=([IO.Directory]::Exists((Join-Path $otherDirectory (Split-Path -Leaf $destination)))); sentinel=$sentinel } | ConvertTo-Json -Depth 10 -Compress
    exit 0
}

$mockPath = Join-Path $WorkDirectory 'mock.json'
$manifestPath = Join-Path $WorkDirectory 'prerequisites.json'
$callsPath = Join-Path $WorkDirectory 'calls.json'
$casesPath = Join-Path $WorkDirectory 'cases.json'
$scriptPath = Join-Path $WorkDirectory 'under-test.ps1'
# PowerShell 5.1 redirection interprets brackets as wildcards. The child still
# executes from the bracketed fixture tree; only its error spool sits outside it.
$stderrPath = Join-Path (Split-Path -Parent $WorkDirectory) 'stderr.txt'

if ($null -ne $scenario.manifest) { $scenario.manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 }
if ($scenario.mock) { $scenario.mock | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mockPath -Encoding UTF8 }
else { Set-Content -LiteralPath $mockPath -Value '{}' -Encoding Ascii }
if ($scenario.cases) { $scenario.cases | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $casesPath -Encoding UTF8 }
Set-Content -LiteralPath $callsPath -Value ''
foreach ($name in @($scenario.createInstallers)) {
    if ($name) { Set-Content -LiteralPath (Join-Path $WorkDirectory $name) -Value 'fixture-installer' -Encoding Ascii }
}
if ($scenario.fixtureFile) {
    Set-Content -LiteralPath (Join-Path $WorkDirectory $scenario.fixtureFile.name) -Value $scenario.fixtureFile.content -Encoding Ascii -NoNewline
}

$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($ProductionSource, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw ('production source failed to parse: ' + $parseErrors[0].Message) }
$statements = @($ast.EndBlock.Statements)
$mainTry = @($statements | Where-Object { $_ -is [System.Management.Automation.Language.TryStatementAst] })[-1]
$functions = @($statements | Where-Object { $_ -is [System.Management.Automation.Language.FunctionDefinitionAst] })
if (-not $mainTry -or $functions.Count -eq 0) { throw 'expected a main try block and helper functions' }
$prefixStart = $statements[0].Extent.StartOffset
$helperPrefix = $source.Substring($prefixStart, $functions[0].Extent.StartOffset - $prefixStart)

$sharedHead = @'
# Each child is a fresh PS 5.1 process; the outer import is not inherited.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$Mock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'mock.json') -Raw | ConvertFrom-Json
$CallsPath = Join-Path $PSScriptRoot 'calls.json'
function Record-Call {
    param([string]$Kind, $Data)
    [pscustomobject]@{ kind = $Kind; data = $Data } | ConvertTo-Json -Compress -Depth 6 | Add-Content -LiteralPath $CallsPath
}
'@
$scenarioOverrides = @'
# Test-only overrides; Start-Process cannot spawn a real process.
# Record the attempt before reading a plan. Missing, empty, and exhausted
# plans record an independent fixture-violation, then throw. A planned
# throwMessage is an intentional query failure, not a fixture violation.
$script:VcIndex = 0; $script:RuntimeIndex = 0; $script:ProcessIndex = 0
function Get-FixturePlan {
    param([string]$Kind, $Plans, [bool]$HasPlans, [int]$Index, $CallData)
    Record-Call $Kind $CallData
    $count = 0
    $entries = @()
    if ($HasPlans -and $null -ne $Plans) {
        $entries = @($Plans)
        $count = $entries.Count
    }
    if (-not $HasPlans -or $null -eq $Plans -or $Index -ge $count) {
        Record-Call 'fixture-violation' @{ queried = $Kind; index = $Index; count = $count }
        throw ("fixture violation: {0} plan missing, empty, or exhausted at index {1}" -f $Kind, $Index)
    }
    return $entries[$Index]
}
function Test-ElevatedAdministrator { return $Mock.admin }
function Get-VcRuntimeState {
    param([string]$RequiredVersion)
    $hasPlans = $null -ne $Mock.PSObject.Properties['vcStates']
    $plans = $null
    if ($hasPlans) { $plans = $Mock.vcStates }
    $state = Get-FixturePlan -Kind 'vc-query' -Plans $plans -HasPlans $hasPlans -Index $script:VcIndex -CallData @{}
    $script:VcIndex++
    $plannedThrow = $state.PSObject.Properties['throwMessage']
    if ($plannedThrow -and $plannedThrow.Value) { throw [string]$plannedThrow.Value }
    return [ordered]@{ ready = $state.ready; requiredVersion = $RequiredVersion; versions = $state.versions; missing = @($state.missing) }
}
function Get-AppRuntimeState {
    param([string]$RequiredVersion)
    $hasPlans = $null -ne $Mock.PSObject.Properties['runtimeStates']
    $plans = $null
    if ($hasPlans) { $plans = $Mock.runtimeStates }
    $state = Get-FixturePlan -Kind 'runtime-query' -Plans $plans -HasPlans $hasPlans -Index $script:RuntimeIndex -CallData @{}
    $script:RuntimeIndex++
    return [ordered]@{ ready = $state.ready; requiredVersion = $RequiredVersion; selectedVersion = $state.selectedVersion }
}
function Start-Process {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$Verb, [switch]$Wait, [switch]$PassThru)
    $hasPlans = $null -ne $Mock.PSObject.Properties['processes']
    $plans = $null
    if ($hasPlans) { $plans = $Mock.processes }
    $plan = Get-FixturePlan -Kind 'process' -Plans $plans -HasPlans $hasPlans -Index $script:ProcessIndex -CallData @{ path = $FilePath; args = @($ArgumentList); verb = $Verb }
    $script:ProcessIndex++
    if ($plan.PSObject.Properties['throwNativeError']) {
        throw (New-Object System.ComponentModel.Win32Exception([int]$plan.throwNativeError))
    }
    return [pscustomobject]@{ ExitCode = [int]$plan.exitCode }
}
'@
$extras = @()
$mockProperties = @()
if ($scenario.mock) { $mockProperties = @($scenario.mock.PSObject.Properties.Name) }
if ($mockProperties -contains 'appx') {
    $extras += @'
function Get-AppxPackage {
    param([string]$Name, [string]$PackageTypeFilter)
    Record-Call 'appx' @{ name = $Name; filter = $PackageTypeFilter }
    return @($Mock.appx.packages)
}
'@
}
if ($mockProperties -contains 'integrity') {
    $extras += @'
function Test-InstallerIntegrity {
    param([string]$Path, [string]$ExpectedSha256)
    $name = Split-Path -Leaf $Path
    if ($Mock.integrity.$name) { return [pscustomobject]@{ Ok = $true; Detail = ('verified: ' + $name) } }
    return [pscustomobject]@{ Ok = $false; Detail = ('mock refusal: ' + $name) }
}
'@
}
if ($mockProperties -contains 'signature') {
    $extras += @'
function Get-AuthenticodeSignature {
    param([string]$LiteralPath)
    Record-Call 'signature' @{ literalPath = $LiteralPath }
    $certificate = $null
    if ($Mock.signature.subject) { $certificate = [pscustomobject]@{ Subject = $Mock.signature.subject } }
    return [pscustomobject]@{ Status = $Mock.signature.status; SignerCertificate = $certificate }
}
'@
}
if ($mockProperties -contains 'crtVersions') {
    # Only the static metadata read changes in the test copy. The original
    # five-DLL iteration, version comparison and result construction still run.
    $detector = @($functions | Where-Object { $_.Name -eq 'Get-VcRuntimeState' })
    $call = '[System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)'
    if ($detector.Count -ne 1 -or ($detector[0].Extent.Text.Split(@($call), [StringSplitOptions]::None).Count -ne 2)) { throw 'CRT metadata seam not unique' }
    $detectorText = $detector[0].Extent.Text.Replace($call, '(Get-TestCrtFileVersion $path)')
    $functions = @($functions | Where-Object { $_.Name -ne 'Get-VcRuntimeState' })
    $extras += $detectorText
    $extras += @'
function Test-Path {
    param([string]$LiteralPath, [string]$PathType)
    return $null -ne $Mock.crtVersions.PSObject.Properties[[IO.Path]::GetFileName($LiteralPath)]
}
function Get-TestCrtFileVersion {
    param([string]$Path)
    $v = [version]$Mock.crtVersions.PSObject.Properties[[IO.Path]::GetFileName($Path)].Value
    return [pscustomobject]@{ FileMajorPart=$v.Major; FileMinorPart=$v.Minor; FileBuildPart=$v.Build; FilePrivatePart=$v.Revision }
}
'@
}

function Read-Calls {
    $calls = @()
    foreach ($line in @(Get-Content -LiteralPath $callsPath)) {
        if ($line.Trim()) { $calls += ($line | ConvertFrom-Json) }
    }
    return $calls
}

if ($scenario.kind -eq 'pure') {
    $runner = @'
$FixtureDirectory = $PSScriptRoot
$PureCases = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cases.json') -Raw | ConvertFrom-Json
$results = @()
foreach ($case in @($PureCases)) {
    $actual = Invoke-Expression $case.expr
    $serialized = if ($null -eq $actual) { 'null' } else { [string](ConvertTo-Json -InputObject $actual -Compress -Depth 6) }
    $results += [pscustomobject]@{ name = $case.name; actual = $serialized }
}
[Console]::Out.WriteLine(($results | ConvertTo-Json -Compress -Depth 6))
'@
    $body = $helperPrefix + "`r`n" + (($functions | ForEach-Object { $_.Extent.Text }) -join "`r`n`r`n") +
        "`r`n" + $sharedHead + "`r`n" + ($extras -join "`r`n") + "`r`n" + $runner
    Set-Content -LiteralPath $scriptPath -Value $body -Encoding Ascii
    $output = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $scriptPath 2> $stderrPath
    if ($LASTEXITCODE -ne 0) { throw ('pure run failed: ' + (Get-Content -LiteralPath $stderrPath -Raw)) }
    $cases = (($output -join "`n") | ConvertFrom-Json)
    [pscustomobject]@{ cases = @($cases); calls = @(Read-Calls) } | ConvertTo-Json -Depth 8 -Compress
    exit 0
}

# Scenario mode inserts test overrides immediately before the main try block.
$insert = $sharedHead + "`r`n" + $scenarioOverrides + "`r`n" + ($extras -join "`r`n")
$manifestArgument = $manifestPath
if ($scenario.relativeManifest) {
    $manifestArgument = 'prerequisites.json'
    $insert += @'

Set-Location -LiteralPath $PSScriptRoot
[Environment]::CurrentDirectory = Split-Path -Parent $PSScriptRoot
'@
}
$body = $source.Substring(0, $mainTry.Extent.StartOffset) + "`r`n" + $insert + "`r`n" + $source.Substring($mainTry.Extent.StartOffset)
Set-Content -LiteralPath $scriptPath -Value $body -Encoding Ascii
$switchArguments = @()
if ($scenario.PSObject.Properties['switches'] -and $scenario.switches) { $switchArguments = @($scenario.switches) }
$arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath) + $switchArguments + @('-ManifestPath', $manifestArgument)
$stdout = & powershell.exe @arguments 2> $stderrPath
[pscustomobject]@{
    exitCode = $LASTEXITCODE
    stdout = (@($stdout) -join "`n")
    stderr = "$(Get-Content -LiteralPath $stderrPath -Raw)"
    calls = @(Read-Calls)
} | ConvertTo-Json -Depth 8 -Compress
