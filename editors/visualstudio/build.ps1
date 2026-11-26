param([string]$NodeDirectory)
$ErrorActionPreference = 'Stop'
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $locator -latest -products * -version '[17.14,)' -requires Microsoft.Component.MSBuild -property installationPath
if (!$vs) { throw 'Install Visual Studio 2022 17.14+ or 2026 with MSBuild and the .NET Framework 4.8 targeting pack.' }
if (!$NodeDirectory) { $NodeDirectory = Join-Path $vs 'MSBuild\Microsoft\VisualStudio\NodeJs' }
if (!(Test-Path "$NodeDirectory\node.exe") -or !(Test-Path "$NodeDirectory\LICENSE")) { throw 'Pass -NodeDirectory pointing to a Windows x64 Node.js 18+ distribution with node.exe and LICENSE.' }
if (!(Test-Path "$PSScriptRoot\..\vscode\node_modules\esbuild")) {
    Push-Location "$PSScriptRoot\..\vscode"
    try { npm ci; if ($LASTEXITCODE) { exit $LASTEXITCODE } } finally { Pop-Location }
}
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$PSScriptRoot\Bolt.VisualStudio.csproj" /restore /nologo /v:minimal /p:Configuration=Release "/p:NodeDirectory=$NodeDirectory"
exit $LASTEXITCODE
