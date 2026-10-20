param(
    [string]$BuildDirectory = "$PSScriptRoot/out/build/release",
    [string]$OutputDirectory = "$PSScriptRoot/out/packages"
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$exe = Join-Path $build 'MarvelNemesis.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build MarvelNemesis first; see docs/BUILDING.md.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'The Visual C++ redistributable files were not found.' }
$crt = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Redist/MSVC') -Directory -Filter '1*' |
    Sort-Object Name -Descending | ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' } |
    Select-Object -First 1
if (-not $crt) { throw 'The x64 Visual C++ redistributable files were not found.' }
$d3d = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$zip = Join-Path $output 'MarvelNemesis-Windows-x64.zip'
if (Test-Path -LiteralPath $zip) { throw "Package already exists: $zip. Choose a new OutputDirectory." }
# A new staging directory and an explicit allowlist keep generated code, data and saves out.
$stage = Join-Path $output ('stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$manifest = @()
function Add-ReleaseFile([string]$Source, [string]$Relative) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required release file is missing: $Source" }
    $destination = Join-Path $stage $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination
    $script:manifest += [pscustomobject]@{path=$Relative.Replace('\','/'); sha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()}
}
Add-ReleaseFile $exe 'MarvelNemesis.exe'
Add-ReleaseFile $d3d 'd3dcompiler_47.dll'
foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    Add-ReleaseFile (Join-Path $crt.FullName $name) $name
    Add-ReleaseFile (Join-Path $crt.FullName $name) "compiler/$name"
}
foreach ($name in @('clang.exe','lld-link.exe','xbe_recomp.exe','MarvelNemesis.lib','LLVM-Apache-2.0-with-exceptions.txt')) {
    Add-ReleaseFile (Join-Path $build "compiler/$name") "compiler/$name"
}
Add-ReleaseFile (Join-Path $build 'compiler/config/game.toml') 'compiler/config/game.toml'
Add-ReleaseFile (Join-Path $build 'compiler/xdk/5849/layout.toml') 'compiler/xdk/5849/layout.toml'
foreach ($name in @('x86_ops.h','x86_context.h')) {
    Add-ReleaseFile (Join-Path $build "compiler/include/cpu/$name") "compiler/include/cpu/$name"
}
foreach ($name in @('cmath','cstdint','cstring','limits','type_traits')) {
    Add-ReleaseFile (Join-Path $build "compiler/include/freestanding/$name") "compiler/include/freestanding/$name"
}
foreach ($name in @('LICENSE','NOTICE')) { Add-ReleaseFile (Join-Path $PSScriptRoot $name) $name }
foreach ($name in @('LICENSE','THIRD_PARTY.txt')) { Add-ReleaseFile (Join-Path $PSScriptRoot "sdk/$name") "sdk/$name" }
foreach ($file in Get-ChildItem -LiteralPath "$PSScriptRoot/docs/licenses" -File) {
    Add-ReleaseFile $file.FullName ("docs/licenses/" + $file.Name)
}
Add-ReleaseFile "$PSScriptRoot/docs/PLAYING.txt" 'README.txt'
Add-ReleaseFile "$PSScriptRoot/docs/ASSETS.txt" 'assets/README.txt'
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($relative in (@($manifest.path) + 'manifest.json')) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $stage $relative), $relative,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $actual = @($archive.Entries | Where-Object Name | ForEach-Object FullName)
    $expected = @($manifest.path) + 'manifest.json'
    if (@(Compare-Object $expected $actual).Count) { throw 'Release archive does not match the allowlist.' }
    if (@($actual | Where-Object { $_ -match '(?i)(^|/)(work|generated|saves|mods|logs|captures)/|\.(xbe|iso|xpr|dds|pdb|obj|cpp)$|^MarvelNemesis\.dll$' }).Count) {
        throw 'A private or game-derived file reached the release archive.'
    }
} finally { $archive.Dispose() }
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Format-List
Write-Output "Package ready: $zip"
