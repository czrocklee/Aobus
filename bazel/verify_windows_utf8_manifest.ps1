param(
    [Parameter(Mandatory=$true)][string]$Mt,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$Manifest,
    [Parameter(Mandatory=$true)][string]$Stamp
)
$ErrorActionPreference = 'Stop'
# Both the selected SDK tool and this verifier are declared action inputs.
# Use the installed SDK/system runtime locations, not relocated executable copies.
& $Mt '-nologo' "-inputresource:$Binary;#1" "-out:$Manifest"
if ($LASTEXITCODE -ne 0) { throw "mt.exe failed with exit $LASTEXITCODE for $Binary" }

$settings = [Xml.XmlReaderSettings]::new()
$settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
$settings.XmlResolver = $null
$reader = [Xml.XmlReader]::Create([IO.Path]::GetFullPath($Manifest), $settings)
try {
    $document = [Xml.XmlDocument]::new()
    $document.XmlResolver = $null
    $document.Load($reader)
} finally {
    $reader.Dispose()
}
$namespaces = [Xml.XmlNamespaceManager]::new($document.NameTable)
$namespaces.AddNamespace('windows', 'http://schemas.microsoft.com/SMI/2019/WindowsSettings')
$values = $document.SelectNodes('//windows:activeCodePage', $namespaces)
if ($values.Count -ne 1 -or $values[0].InnerText.Trim() -cne 'UTF-8') {
    throw "Resource #1 must contain exactly one UTF-8 activeCodePage: $Binary"
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Stamp), "UTF-8`n", [Text.UTF8Encoding]::new($false))
