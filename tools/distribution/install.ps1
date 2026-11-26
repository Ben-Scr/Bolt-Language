#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$release = '0.1.0-F5'
$prefix = $null
$withCode = $false
$noPath = $false
$plan = $false

function Show-Help {
    Write-Output 'install.bat [--prefix "C:\Path With Spaces\Bolt"] [--with-vscode] [--no-path] [--plan]'
    Write-Output 'Installs the SDK for the native Windows architecture under your user account.'
}
for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        '--help'        { Show-Help; exit 0 }
        '/?'            { Show-Help; exit 0 }
        '--with-vscode' { $withCode = $true }
        '--no-path'     { $noPath = $true }
        '--plan'        { $plan = $true }
        '--prefix' {
            $i++
            if ($i -ge $args.Count -or !$args[$i] -or $args[$i] -like '--*') {
                [Console]::Error.WriteLine('--prefix requires an installation directory.'); exit 2
            }
            $prefix = [string]$args[$i]
        }
        default { [Console]::Error.WriteLine("Unknown option: $($args[$i])"); Show-Help; exit 2 }
    }
}

function Get-BoltArchitecture {
    # Query the OS even when this PowerShell process is running under emulation.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BoltNativeArchitecture {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool IsWow64Process2(IntPtr process, out ushort machine, out ushort nativeMachine);
}
'@
    try {
        [uint16]$emulated = 0
        [uint16]$native = 0
        if ([BoltNativeArchitecture]::IsWow64Process2([IntPtr](-1), [ref]$emulated, [ref]$native)) {
            switch ([int]$native) {
                34404 { return 'x64' }
                43620 { return 'arm64' }
                default { throw ('No bundled SDK for native Windows machine 0x{0:x4}.' -f $native) }
            }
        }
    } catch [System.Management.Automation.MethodInvocationException] {
        if ($_.Exception.InnerException -isnot [EntryPointNotFoundException]) { throw }
    }
    $arch = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
    if (!$arch) { $arch = $env:PROCESSOR_ARCHITEW6432 }
    if (!$arch) { $arch = $env:PROCESSOR_ARCHITECTURE }
    switch ($arch) {
        'AMD64' { return 'x64' }
        'ARM64' { return 'arm64' }
        default { throw "No bundled Windows SDK for $arch." }
    }
}

function Assert-BundleFile([string]$Relative, [string[]]$Sums) {
    $path = Join-Path $PSScriptRoot $Relative
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Extract the whole Bolt.zip. Missing file: $path" }
    $expression = '^([0-9a-f]{64})  ' + [regex]::Escape($Relative) + '$'
    $record = @($Sums | Where-Object { $_ -cmatch $expression })
    if ($record.Count -ne 1) { throw "Missing or duplicate bundle checksum: $Relative" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $record[0].Substring(0, 64)) {
        throw "Bundle checksum mismatch: $Relative"
    }
    return $path
}

$stage = $null
$status = 0
try {
    $arch = Get-BoltArchitecture
    $sdkName = "sdk/Bolt-$release-Windows-$arch.zip"
    $editorName = "editor/Bolt-$release-VSCode.vsix"
    $sums = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'SHA256SUMS') -Encoding UTF8
    $archive = Assert-BundleFile $sdkName $sums
    if ($withCode) { $vsix = Assert-BundleFile $editorName $sums }
    if ($plan) {
        Write-Output "Install SDK $archive"
        if ($prefix) { Write-Output "Prefix $prefix" } else { Write-Output 'Prefix user default' }
        if ($withCode) { Write-Output "VS Code extension $vsix" }
        exit 0
    }
    if ($withCode -and !(Get-Command code -CommandType Application -ErrorAction SilentlyContinue)) {
        foreach ($parent in @($env:LOCALAPPDATA, $env:ProgramFiles)) {
            if (!$parent) { continue }
            $subdir = if ($parent -eq $env:LOCALAPPDATA) { 'Programs\Microsoft VS Code\bin' } else { 'Microsoft VS Code\bin' }
            $dir = Join-Path $parent $subdir
            if (Test-Path -LiteralPath (Join-Path $dir 'code.cmd') -PathType Leaf) {
                $env:Path = $dir + ';' + $env:Path
                break
            }
        }
        if (!(Get-Command code -CommandType Application -ErrorAction SilentlyContinue)) {
            throw 'The --with-vscode option requires the VS Code command code.'
        }
    }
    $candidate = [IO.Path]::Combine([IO.Path]::GetTempPath(), ('bolt-install-' + [guid]::NewGuid().ToString('N')))
    $stage = (New-Item -Path $candidate -ItemType Directory).FullName
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $stage)
    $sdk = Join-Path $stage "Bolt-Windows-$arch"
    $options = @{ Action='Install'; Source=$sdk; SkipVSCode=(!$withCode); NoPath=$noPath }
    if ($prefix) { $options.Prefix = [IO.Path]::GetFullPath($prefix) }
    if ($withCode) {
        $inner = Get-FileHash -LiteralPath (Join-Path $sdk 'Bolt-VSCode.vsix') -Algorithm SHA256
        if ($inner.Hash -ne (Get-FileHash -LiteralPath $vsix -Algorithm SHA256).Hash) {
            throw 'The bundle and SDK contain different VS Code extensions.'
        }
    }
    $global:LASTEXITCODE = 0
    & (Join-Path $sdk 'sdk-setup.ps1') @options
    if ($LASTEXITCODE -ne 0) { $status = 1 }
} catch {
    [Console]::Error.WriteLine('Bolt installation: ' + $_.Exception.Message)
    $status = 1
} finally {
    if ($stage) {
        try { Remove-Item -LiteralPath $stage -Recurse -Force }
        catch { [Console]::Error.WriteLine('Temporary folder cleanup failed: ' + $stage); $status = 1 }
    }
}
exit $status
