#requires -Version 5.1
[CmdletBinding()]
param([string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\Bolt'))
$ErrorActionPreference = 'Stop'
try {
    $prefixPath = [IO.Path]::GetFullPath($Prefix).TrimEnd('\')
    $marker = Join-Path $prefixPath '.bolt-install.json'
    if (-not (Test-Path -LiteralPath $marker)) { throw 'Not a managed Bolt installation.' }
    $data = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    if ($data.product -ne 'bolt-user-install-v1' -or $data.prefix -ne $prefixPath) { throw 'Invalid installation marker.' }
    # Preflight every path before changing PATH or deleting a file.
    foreach ($entry in $data.files.PSObject.Properties) {
        $p = [IO.Path]::GetFullPath((Join-Path $prefixPath $entry.Name))
        if (-not $p.StartsWith($prefixPath + '\', 'OrdinalIgnoreCase')) { throw 'Invalid payload path.' }
        $current = $p
        while ($current) {
            if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Refusing reparse point: $current" }
            $parent = Split-Path -Parent $current
            if ($parent -eq $current) { break }
            $current = $parent
        }
    }
    if ($data.environment) {
        foreach ($entry in $data.environment.PSObject.Properties) {
            if ($entry.Name -notin @('BOLT_LINKER', 'BOLT_LLVM_LIBRARY')) { throw 'Invalid owned environment variable.' }
        }
    }
    if ($data.pathAdded) {
        $bin = Join-Path $prefixPath 'bin'
        $path = [Environment]::GetEnvironmentVariable('Path', 'User')
        $parts = @($path -split ';' | Where-Object { $_.TrimEnd('\') -ine $bin })
        [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
    }
    if ($data.environment) {
        foreach ($entry in $data.environment.PSObject.Properties) {
            $current = [Environment]::GetEnvironmentVariable($entry.Name, 'User')
            if ($current -ceq $entry.Value.value) {
                [Environment]::SetEnvironmentVariable($entry.Name, $entry.Value.previousValue, 'User')
            } else { Write-Host "Preserved changed environment variable: $($entry.Name)" }
        }
    }
    $reg = 'HKCU:\Software\BoltLanguage'
    $currentCompiler = (Get-ItemProperty -Path $reg -Name CompilerPath -ErrorAction SilentlyContinue).CompilerPath
    if ($data.registryOwned -and $currentCompiler -eq (Join-Path $prefixPath 'libexec\bolt.exe')) {
        if ($data.previousCompiler) { Set-ItemProperty -Path $reg -Name CompilerPath -Value $data.previousCompiler }
        else { Remove-ItemProperty -Path $reg -Name CompilerPath -ErrorAction SilentlyContinue }
    }
    . (Join-Path $PSScriptRoot 'file-icons.ps1')
    Restore-BoltFileIcon $data.iconRegistration
    foreach ($entry in $data.files.PSObject.Properties) {
        $p = Join-Path $prefixPath $entry.Name
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            if ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -eq $entry.Value) { Remove-Item -LiteralPath $p -Force }
            else { Write-Host "Preserved modified file: $p" }
        }
    }
    Remove-Item -LiteralPath $marker -Force
    Get-ChildItem -LiteralPath $prefixPath -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object {
        if (-not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $_.FullName -Force }
    }
    if (-not (Get-ChildItem -LiteralPath $prefixPath -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $prefixPath -Force }
    Write-Host 'Uninstalled Bolt. Reopen terminals/IntelliJ IDEA to refresh PATH.'
} catch { Write-Error $_; exit 1 }
