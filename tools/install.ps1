#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\Bolt'),
    [string]$SourceBin = '',
    [switch]$NoBuild,
    [switch]$NoPath,
    [switch]$CheckDependencies,
    [switch]$AllowMissingLLVM,
    [string]$LlvmLibrary = '',
    [string]$Linker = ''
)
$ErrorActionPreference = 'Stop'
$nativePayload = @('bolt.exe', 'BoltCore.lib', 'BoltRuntime.dll', 'BoltRuntime.lib', 'BoltCompiler.dll', 'BoltCompiler.lib')
. (Join-Path $PSScriptRoot 'file-icons.ps1')
. (Join-Path $PSScriptRoot 'windows-dependencies.ps1')
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$prefixPath = [IO.Path]::GetFullPath($Prefix).TrimEnd('\')
$markerPath = Join-Path $prefixPath '.bolt-install.json'
function Assert-SafePath([string]$path) {
    $current = $path
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing reparse-point path: $current" }
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
function Hash([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Write-Marker($data) { $data | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $markerPath -Encoding UTF8 }
function Invoke-InstallTool([string]$FilePath, [string[]]$Arguments, [int]$TimeoutSeconds = 120) {
    $result = Invoke-BoltDependencyCommand -FilePath $FilePath -ArgumentList $Arguments -WorkingDirectory $root -TimeoutSeconds $TimeoutSeconds
    if ($result.Output) { Write-Host $result.Output.TrimEnd() }
    if ($result.ExitCode -ne 0) { throw "Command failed (exit $($result.ExitCode)): $FilePath" }
}
function Test-InstallNativePayload($Dependencies) {
    foreach ($name in $nativePayload) {
        if (-not (Test-Path -LiteralPath (Join-Path $SourceBin $name) -PathType Leaf)) { throw "Missing $name in $SourceBin. Select a complete built distribution with -SourceBin." }
    }
    $probeRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('bolt-install-probe-' + [Guid]::NewGuid().ToString('N'))))
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if (-not $probeRoot.StartsWith($tempParent + '\', 'OrdinalIgnoreCase')) { throw 'Invalid installer probe directory.' }
    try {
        New-Item -ItemType Directory -Path $probeRoot | Out-Null
        $source = Join-Path $probeRoot 'Probe.bolt'
        'static class Program { public: void Main() { Print(42); } }' | Set-Content -LiteralPath $source -Encoding UTF8
        $compiler = Join-Path $SourceBin 'bolt.exe'
        $backends = @()
        if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() -eq 'X64') { $backends += 'fast' }
        if ($Dependencies.LlvmLibrary) { $backends += 'llvm' }
        if (-not $backends.Count) { throw 'No verified direct native backend is available for this host.' }
        foreach ($backend in $backends) {
            $target = Join-Path $probeRoot ($backend + '.exe')
            $arguments = @('compile', $source, '--backend', $backend, '--require-native', '--linker', $Dependencies.Linker, '--runtime-dir', $SourceBin, '-o', $target, '--no-timings')
            if ($backend -eq 'llvm') { $arguments += @('--llvm', $Dependencies.LlvmLibrary, '--release') }
            Invoke-InstallTool $compiler $arguments
            $run = Invoke-BoltDependencyCommand -FilePath $target -ArgumentList @() -WorkingDirectory $probeRoot -TimeoutSeconds 30
            if ($run.ExitCode -ne 0 -or $run.Output.Trim() -ne '42') { throw "$backend native runtime verification failed: $($run.Output)" }
            Write-Host "[OK] $backend native compile/link/run (compiler, linker and matching BoltRuntime)"
        }
    } finally {
        if ((Test-Path -LiteralPath $probeRoot) -and $probeRoot.StartsWith($tempParent + '\', 'OrdinalIgnoreCase')) {
            Assert-SafePath $probeRoot
            Remove-Item -LiteralPath $probeRoot -Recurse -Force
        }
    }
}
try {
    Assert-SafePath $prefixPath
    if ($prefixPath -eq $root -or $prefixPath.StartsWith($root + '\', 'OrdinalIgnoreCase') -or
        $root.StartsWith($prefixPath + '\', 'OrdinalIgnoreCase') -or
        $prefixPath -eq [IO.Path]::GetPathRoot($prefixPath).TrimEnd('\') -or
        $prefixPath -eq $env:USERPROFILE -or $prefixPath -match '[;\r\n%!]') {
        throw 'Use a separate dedicated per-user installation directory (without ;, %, ! or line breaks).'
    }
    $buildDir = Join-Path $root 'build\install-windows'
    $buildRequired = -not $SourceBin -and -not $NoBuild
    if (-not $SourceBin) {
        $SourceBin = Join-Path $buildDir 'bin\Release'
        if (-not (Test-Path -LiteralPath (Join-Path $SourceBin 'bolt.exe')) -and (Test-Path -LiteralPath (Join-Path $buildDir 'bin\bolt.exe'))) {
            $SourceBin = Join-Path $buildDir 'bin'
        }
    }
    $SourceBin = [IO.Path]::GetFullPath($SourceBin)
    $dependencyOptions = @{ Root=$root; SourceBin=$SourceBin; Prefix=$prefixPath; BuildRequired=$buildRequired; AllowMissingLLVM=$AllowMissingLLVM; LlvmLibrary=$LlvmLibrary; Linker=$Linker }
    $dependencies = Test-BoltWindowsDependencies @dependencyOptions
    if (-not $dependencies.Success) { throw ("Dependency check failed. Installation has not been changed.`n" + ($dependencies.Errors -join "`n")) }
    if ($CheckDependencies) {
        if ($dependencies.CompilerVerified) {
            Test-InstallNativePayload $dependencies
            Write-Host 'Dependency checks passed. No installation, user environment or registry settings were changed.'
        } else {
            Write-Host 'Pre-build dependencies found. LLVM/runtime validation still requires a compiled Bolt distribution; run install.bat to build and verify it.'
        }
        exit 0
    }
    $previous = $null
    if (Test-Path -LiteralPath $markerPath) {
        $previous = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
        if ($previous.product -ne 'bolt-user-install-v1' -or $previous.prefix -ne $prefixPath) { throw 'Invalid installation marker.' }
    } elseif ((Test-Path -LiteralPath $prefixPath) -and (Get-ChildItem -LiteralPath $prefixPath -Force | Select-Object -First 1)) {
        throw 'Refusing to install over an existing, unowned nonempty folder.'
    }
    # CMake is the single compiler build description; no checked-in IDE solution.
    if ($buildRequired) {
        Invoke-InstallTool $dependencies.CMake @('-S', $root, '-B', $buildDir, '-DCMAKE_BUILD_TYPE=Release', '-DBOLT_BUILD_TESTS=OFF', '-DBOLT_BUILD_EMBEDDING=OFF', ('-DPython3_EXECUTABLE=' + $dependencies.Python)) 300
        # Use one MSBuild worker: parallel workers can inherit duplicate PATH/Path keys.
        Invoke-InstallTool $dependencies.CMake @('--build', $buildDir, '--config', 'Release', '--parallel', '1') 1800
        $SourceBin = Join-Path $buildDir 'bin\Release'
        if (-not (Test-Path -LiteralPath (Join-Path $SourceBin 'bolt.exe'))) { $SourceBin = Join-Path $buildDir 'bin' }
    }
    $SourceBin = [IO.Path]::GetFullPath($SourceBin)
    foreach ($name in $nativePayload) {
        if (-not (Test-Path -LiteralPath (Join-Path $SourceBin $name))) { throw "Missing $name in $SourceBin. Build the complete CMake Release distribution with an MSVC-compatible toolchain first." }
    }
    if ($buildRequired -or -not $dependencies.CompilerVerified) {
        $dependencyOptions.SourceBin = $SourceBin
        $dependencyOptions.BuildRequired = $false
        $dependencies = Test-BoltWindowsDependencies @dependencyOptions
        if (-not $dependencies.Success -or -not $dependencies.CompilerVerified) {
            throw ("Built distribution failed dependency validation. Installation has not been changed.`n" + ($dependencies.Errors -join "`n"))
        }
    }
    Test-InstallNativePayload $dependencies
    $stage = Join-Path ([IO.Path]::GetTempPath()) ('bolt-stage-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $stage 'bin'), (Join-Path $stage 'libexec\include'), (Join-Path $stage 'share') -Force | Out-Null
    foreach ($name in $nativePayload) {
        Copy-Item -LiteralPath (Join-Path $SourceBin $name) -Destination (Join-Path $stage 'libexec')
    }
    Copy-Item -LiteralPath (Join-Path $root 'include\bolt') -Destination (Join-Path $stage 'libexec\include') -Recurse
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE'), (Join-Path $root 'README.md') -Destination (Join-Path $stage 'share')
    Copy-Item -LiteralPath (Join-Path $root 'docs\licenses') -Destination (Join-Path $stage 'share\licenses') -Recurse
    Copy-Item -LiteralPath (Join-Path $root 'stdlib') -Destination (Join-Path $stage 'share\stdlib') -Recurse
    Copy-Item -LiteralPath (Join-Path $root 'assets\bolt-file.ico') -Destination (Join-Path $stage 'share\bolt-file.ico')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bolt-watch.py') -Destination (Join-Path $stage 'libexec\bolt-watch.py')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bolt.cmd') -Destination (Join-Path $stage 'bin\bolt.cmd')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination (Join-Path $stage 'uninstall.ps1')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'file-icons.ps1') -Destination (Join-Path $stage 'file-icons.ps1')
    '@echo off', 'setlocal', 'set "PSModulePath="', 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -Prefix "%~dp0." %*', 'exit /b %errorlevel%' | Set-Content (Join-Path $stage 'uninstall.bat') -Encoding ASCII
    $files = @{}
    foreach ($p in Get-ChildItem -LiteralPath $stage -Recurse -File) {
        $relative = $p.FullName.Substring($stage.Length + 1)
        $dest = Join-Path $prefixPath $relative
        Assert-SafePath $dest
        if (Test-Path -LiteralPath $dest) {
            $old = if ($previous) { $previous.files.PSObject.Properties[$relative] } else { $null }
            if (-not $old -or (Hash $dest) -ne $old.Value) { throw "Refusing to overwrite an unowned or modified file: $dest" }
        }
        $files[$relative] = Hash $p.FullName
    }
    # Detect running compilers and other locked payloads before updating any file.
    foreach ($relative in $files.Keys) {
        $dest = Join-Path $prefixPath $relative
        if ((Test-Path -LiteralPath $dest -PathType Leaf) -and (Hash $dest) -ne $files[$relative]) {
            $handle = $null
            try {
                $handle = [IO.File]::Open($dest, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
            } catch {
                throw "Cannot update file in use or write-protected: $dest. Close programs using Bolt (including IntelliJ), then rerun the installer. No installed files have been changed."
            } finally { if ($handle) { $handle.Dispose() } }
        }
    }
    New-Item -ItemType Directory -Path $prefixPath -Force | Out-Null
    foreach ($relative in $files.Keys) {
        $dest = Join-Path $prefixPath $relative
        # Keep identical files in place, including a compiler currently used by the IDE.
        if ((Test-Path -LiteralPath $dest -PathType Leaf) -and (Hash $dest) -eq $files[$relative]) { continue }
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $stage $relative) -Destination $dest -Force
    }
    $versionHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'include/bolt/Version.hpp')
    $release = [regex]::Match($versionHeader, '(?m)^#define BOLT_VERSION_STRING "([0-9.]+)"').Groups[1].Value
    if (!$release) { throw 'Invalid SDK version header.' }
    $manifest = @{ product='bolt-user-install-v1'; version=$release; prefix=$prefixPath; files=$files; pathAdded=$false; registryOwned=$false; previousCompiler=$null; iconRegistration=$null; environment=@{} }
    if ($previous) {
        $manifest.pathAdded = $previous.pathAdded
        $manifest.registryOwned = $previous.registryOwned
        $manifest.previousCompiler = $previous.previousCompiler
        $manifest.iconRegistration = $previous.iconRegistration
        if ($previous.environment) {
            foreach ($entry in $previous.environment.PSObject.Properties) { $manifest.environment[$entry.Name] = $entry.Value }
        }
    }
    Write-Marker $manifest
    # Persist only verified backend paths so direct bolt.exe/IntelliJ launches work.
    # Record ownership before mutation, preserving later user edits on uninstall.
    foreach ($setting in @(@{Name='BOLT_LINKER'; Value=$dependencies.Linker; Explicit=[bool]$Linker}, @{Name='BOLT_LLVM_LIBRARY'; Value=$dependencies.LlvmLibrary; Explicit=[bool]$LlvmLibrary})) {
        if (-not $setting.Value) { continue }
        $current = [Environment]::GetEnvironmentVariable($setting.Name, 'User')
        $owned = $manifest.environment[$setting.Name]
        if ($owned -and $current -cne $owned.value -and -not $setting.Explicit) {
            Write-Host "Preserved $($setting.Name) changed since the previous installation."
            continue
        }
        if (-not $owned -and $current -ceq $setting.Value) { continue }
        if (-not $owned -or $current -cne $owned.value) {
            $owned = @{ previousValue=$current; value=$setting.Value }
        } else { $owned.value = $setting.Value }
        $manifest.environment[$setting.Name] = $owned
        Write-Marker $manifest
        [Environment]::SetEnvironmentVariable($setting.Name, $setting.Value, 'User')
        Write-Host "Configured $($setting.Name): $($setting.Value)"
    }
    if (-not $NoPath) {
        $binPath = Join-Path $prefixPath 'bin'
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        $parts = @($userPath -split ';' | Where-Object { $_ })
        if (-not ($parts | Where-Object { $_.TrimEnd('\') -ieq $binPath })) {
            [Environment]::SetEnvironmentVariable('Path', (($parts + $binPath) -join ';'), 'User')
            $manifest.pathAdded = $true
        }
    }
    $reg = 'HKCU:\Software\BoltLanguage'
    if (-not $manifest.registryOwned) {
        $manifest.previousCompiler = (Get-ItemProperty -Path $reg -Name CompilerPath -ErrorAction SilentlyContinue).CompilerPath
    }
    New-Item -Path $reg -Force | Out-Null
    New-ItemProperty -Path $reg -Name CompilerPath -Value (Join-Path $prefixPath 'libexec\bolt.exe') -PropertyType String -Force | Out-Null
    $manifest.registryOwned = $true
    Write-Marker $manifest
    # Move an older .blt icon registration to .bolt without losing the user's old setting.
    if ($manifest.iconRegistration -and $manifest.iconRegistration.extension -ne '.bolt') {
        Restore-BoltFileIcon $manifest.iconRegistration
        $manifest.iconRegistration = $null
        Write-Marker $manifest
    }
    # Set only the file icon, leaving the user's default application untouched.
    $extensionKeyPath = 'Software\Classes\.bolt'
    $iconKeyPath = $extensionKeyPath + '\DefaultIcon'
    $iconValue = '"' + (Join-Path $prefixPath 'share\bolt-file.ico') + '",0'
    $extensionKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($extensionKeyPath)
    $extensionKeyExisted = $null -ne $extensionKey
    if ($extensionKey) { $extensionKey.Dispose() }
    $iconKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($iconKeyPath)
    try {
        $iconKeyExisted = $null -ne $iconKey
        $iconValueExisted = $iconKey -and ($iconKey.GetValueNames() -contains '')
        $currentIcon = $null
        $currentIconKind = $null
        if ($iconValueExisted) {
            # Assign directly to preserve empty array-valued registry entries.
            $currentIcon = $iconKey.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $currentIconKind = $iconKey.GetValueKind('').ToString()
        }
    } finally { if ($iconKey) { $iconKey.Dispose() } }
    $iconChanged = $manifest.iconRegistration -and
        (-not $iconValueExisted -or $currentIconKind -ne 'String' -or $currentIcon -ne $manifest.iconRegistration.value)
    if ($iconChanged) {
        Write-Host 'Preserved the .bolt file icon changed since the previous Bolt installation.'
    } else {
        if (-not $manifest.iconRegistration) {
            $manifest.iconRegistration = @{
                extension='.bolt'; value=$iconValue; previousValue=$currentIcon; previousKind=$currentIconKind
                previousValueExisted=[bool]$iconValueExisted; keyCreated=(-not $iconKeyExisted)
                extensionKeyCreated=(-not $extensionKeyExisted)
            }
        }
        # Save the original registration before changing it, including on upgrades.
        Write-Marker $manifest
        $iconKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($iconKeyPath)
        try { $iconKey.SetValue('', $iconValue, [Microsoft.Win32.RegistryValueKind]::String) }
        finally { $iconKey.Dispose() }
    }
    # Refresh environment notifications without changing machine-wide PATH.
    if (-not ('BoltEnvironment' -as [type])) {
        Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class BoltEnvironment { [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, UIntPtr w, string l, uint f, uint t, out UIntPtr r); [DllImport("shell32.dll")] public static extern void SHChangeNotify(uint e, uint f, IntPtr a, IntPtr b); }'
    }
    [BoltEnvironment]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    $result = [UIntPtr]::Zero
    [void][BoltEnvironment]::SendMessageTimeout([IntPtr]0xffff, 0x1a, [UIntPtr]::Zero, 'Environment', 2, 3000, [ref]$result)
    Write-Host "Installed Bolt to $prefixPath"
    if (-not $dependencies.LlvmLibrary) { Write-Host 'LLVM was explicitly omitted: optimized native Release and LLVM debug/JIT builds remain unavailable; automatic C++ fallback may still occur.' }
    Write-Host 'Close and reopen terminals/IntelliJ IDEA, then run: bolt help'
    Write-Host ('For this PowerShell session: $env:Path += '';{0}''' -f (Join-Path $prefixPath 'bin'))
} catch {
    Write-Error $_
    exit 1
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage)) {
        $stageFullPath = [IO.Path]::GetFullPath($stage)
        $stageTempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
        if ($stageFullPath.StartsWith($stageTempParent + '\bolt-stage-', 'OrdinalIgnoreCase')) {
            Assert-SafePath $stageFullPath
            Remove-Item -LiteralPath $stageFullPath -Recurse -Force
        }
    }
}
