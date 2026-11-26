#requires -Version 5.1
# Windows installer preflight. Only the current process environment and disposable
# probe files may be changed here; installation/registry ownership stays in install.ps1.

function Get-BoltDependencyEnvironmentValue([string]$Name, [string]$Scope) {
    return [Environment]::GetEnvironmentVariable($Name, $Scope)
}

function Get-BoltDependencyEnvironment([string]$Name, [string]$Scope = '') {
    $scopes = if ($Scope) { @($Scope) } else { @('Process', 'User', 'Machine') }
    foreach ($level in $scopes) {
        $value = Get-BoltDependencyEnvironmentValue $Name $level
        if (-not [string]::IsNullOrWhiteSpace($value)) { return $value.Trim().Trim('"') }
    }
    return ''
}

function Resolve-BoltDependencyCommand([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name)) { return '' }
    $candidate = [Environment]::ExpandEnvironmentVariables($Name.Trim().Trim('"'))
    if ([IO.Path]::IsPathRooted($candidate) -or $candidate.Contains('\') -or $candidate.Contains('/')) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return [IO.Path]::GetFullPath($candidate) }
        return ''
    }
    $command = Get-Command -Name $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and (Test-Path -LiteralPath $command.Source -PathType Leaf)) { return $command.Source }
    return ''
}

function ConvertTo-BoltDependencyArgument([string]$Value) {
    # Windows CRT argument quoting, including empty arguments and trailing slashes.
    if ($Value -and $Value -notmatch '[\s"]') { return $Value }
    $quoted = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $quoted = [regex]::Replace($quoted, '(\\+)$', '$1$1')
    return '"' + $quoted + '"'
}

function Initialize-BoltDependencyProcessEnvironment($StartInfo) {
    # Codex/PowerShell can inherit both PATH and Path in the native environment.
    # A managed, case-insensitive map emits each name once for CMake/MSBuild.
    $variables = @{}
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) { $variables[[string]$entry.Key] = [string]$entry.Value }
    $StartInfo.EnvironmentVariables.Clear()
    foreach ($name in $variables.Keys) {
        # CMake/MSBuild can synthesize a second "Path" when the inherited spelling
        # is "PATH". Emit this variable using Windows' conventional spelling too.
        $canonicalName = if ($name -ieq 'PATH') { 'Path' } else { $name }
        $StartInfo.EnvironmentVariables[$canonicalName] = $variables[$name]
    }
}

function Invoke-BoltDependencyProcess {
    param([string]$FilePath, [string[]]$Arguments = @(), [string]$WorkingDirectory = '', [int]$TimeoutSeconds = 30, [string]$CommandLine = '')
    $process = New-Object Diagnostics.Process
    try {
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $FilePath
        $start.Arguments = if ($CommandLine) { $CommandLine } else { (($Arguments | ForEach-Object { ConvertTo-BoltDependencyArgument $_ }) -join ' ') }
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        Initialize-BoltDependencyProcessEnvironment $start
        if ($WorkingDirectory) { $start.WorkingDirectory = $WorkingDirectory }
        $process.StartInfo = $start
        if (-not $process.Start()) { throw "Could not start $FilePath" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            # Kill the probe process tree so a timed-out linker/compiler cannot retain
            # handles to the disposable directory. No shell command is constructed.
            $taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
            if (Test-Path -LiteralPath $taskkill) {
                $stop = New-Object Diagnostics.ProcessStartInfo
                $stop.FileName = $taskkill
                $stop.Arguments = '/PID ' + $process.Id + ' /T /F'
                $stop.UseShellExecute = $false
                $stop.CreateNoWindow = $true
                $stop.RedirectStandardOutput = $true
                $stop.RedirectStandardError = $true
                $killer = [Diagnostics.Process]::Start($stop)
                if ($killer) { [void]$killer.WaitForExit(5000); $killer.Dispose() }
            }
            if (-not $process.HasExited) { $process.Kill() }
            [void]$process.WaitForExit(5000)
            return [pscustomobject]@{ ExitCode = -1; StdOut = ''; StdErr = "Timed out after $TimeoutSeconds seconds."; TimedOut = $true }
        }
        return [pscustomobject]@{ ExitCode = $process.ExitCode; StdOut = $stdout.GetAwaiter().GetResult(); StdErr = $stderr.GetAwaiter().GetResult(); TimedOut = $false }
    } catch {
        return [pscustomobject]@{ ExitCode = -1; StdOut = ''; StdErr = $_.Exception.Message; TimedOut = $false }
    } finally { $process.Dispose() }
}

function Invoke-BoltDependencyCommand {
    param([string]$FilePath, [string[]]$ArgumentList = @(), [string]$WorkingDirectory = '', [int]$TimeoutSeconds = 30)
    $probe = Invoke-BoltDependencyProcess -FilePath $FilePath -Arguments $ArgumentList -WorkingDirectory $WorkingDirectory -TimeoutSeconds $TimeoutSeconds
    return [pscustomobject]@{ ExitCode = $probe.ExitCode; Output = $probe.StdOut + $probe.StdErr; TimedOut = $probe.TimedOut }
}

function Get-BoltDependencyLlvmCandidates([string]$SourceBin, [string]$Prefix, [string]$VisualStudio = '') {
    $directories = New-Object 'System.Collections.Generic.List[string]'
    foreach ($directory in @($SourceBin, $(if ($Prefix) { Join-Path $Prefix 'libexec' }))) {
        if ($directory) { [void]$directories.Add($directory) }
    }
    foreach ($directory in ($env:Path -split ';')) { if ($directory) { [void]$directories.Add($directory.Trim('"')) } }
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Programs' }))) {
        if ($base) {
            [void]$directories.Add((Join-Path $base 'LLVM\bin'))
            [void]$directories.Add((Join-Path $base 'LLVM\lib'))
        }
    }
    if ($VisualStudio) {
        [void]$directories.Add((Join-Path $VisualStudio 'VC\Tools\Llvm\x64\bin'))
        [void]$directories.Add((Join-Path $VisualStudio 'VC\Tools\Llvm\bin'))
    }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($directory in $directories) {
        foreach ($name in @('LLVM-C.dll', 'libLLVM.dll', 'LLVM.dll')) {
            try {
                $candidate = [IO.Path]::GetFullPath((Join-Path $directory $name))
                if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and $seen.Add($candidate)) { $candidate }
            } catch { # Ignore malformed or stale PATH entries; explicit overrides are checked separately.
            }
        }
    }
}

function Test-BoltWindowsDependencies {
    [CmdletBinding()]
    param(
        [string]$Root,
        [string]$SourceBin,
        [string]$Prefix,
        [bool]$BuildRequired,
        [switch]$AllowMissingLLVM,
        [string]$LlvmLibrary,
        [string]$Linker
    )
    $errors = New-Object 'System.Collections.Generic.List[string]'
    $result = [ordered]@{ Success = $false; Errors = @(); CMake = ''; Python = ''; Linker = ''; LlvmLibrary = ''; Cxx = ''; CompilerVerified = $false }
    $probeDirectory = ''
    $probeRoot = ''
    function Report-BoltDependency([string]$Kind, [string]$Message) { Write-Host "[$Kind] $Message" }
    function Missing-BoltDependency([string]$Message) { [void]$errors.Add($Message); Report-BoltDependency 'MISSING' $Message }
    function Short-BoltDependencyError($Probe) {
        $detail = (($Probe.StdErr + "`n" + $Probe.StdOut).Trim() -replace '[\r\n]+', ' ')
        if ($detail.Length -gt 700) { $detail = $detail.Substring(0, 700) + '...' }
        if (-not $detail) { $detail = "exit $($Probe.ExitCode)" }
        return $detail
    }
    try {
        # Existing terminals may predate an installer or setx invocation. Read
        # explicit overrides from all scopes without persisting any changes.
        $llvmOverride = if ($LlvmLibrary) { $LlvmLibrary } else { Get-BoltDependencyEnvironment 'BOLT_LLVM_LIBRARY' }
        $linkerOverride = if ($Linker) { $Linker } else { Get-BoltDependencyEnvironment 'BOLT_LINKER' }

        $visualStudio = ''
        $vswhere = if (${env:ProgramFiles(x86)}) { Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' } else { '' }
        if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            $vs = Invoke-BoltDependencyProcess $vswhere @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
            if ($vs.ExitCode -eq 0) { $visualStudio = ($vs.StdOut -split '[\r\n]+' | Where-Object { $_ } | Select-Object -First 1) }
        }
        if (-not $visualStudio -and $env:VSINSTALLDIR) { $visualStudio = $env:VSINSTALLDIR }
        if ($visualStudio) {
            $devCommand = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
            if ((Test-Path -LiteralPath $devCommand -PathType Leaf) -and $devCommand -notmatch '["%!\r\n]') {
                $cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
                # cmd /s /c requires its own outer quotes; this fixed command only
                # invokes the located VS script and captures its process environment.
                try {
                    $line = '/d /s /c "call "' + $devCommand + '" -no_logo -arch=x64 -host_arch=x64 >nul && set"'
                    $environmentProbe = Invoke-BoltDependencyProcess -FilePath $cmd -CommandLine $line -TimeoutSeconds 30
                    if ($environmentProbe.ExitCode -eq 0) {
                        foreach ($line in ($environmentProbe.StdOut -split '[\r\n]+')) {
                            if ($line -match '^([^=]+)=(.*)$') {
                                # Keep the hosting PowerShell runtime's module discovery
                                # and policy. A VS shell inherited from another PowerShell
                                # edition must not break later cmdlet autoloading.
                                if ($matches[1] -in @('PSModulePath', 'PSModuleAnalysisCachePath', 'PSExecutionPolicyPreference', 'PSHOME')) { continue }
                                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
                            }
                        }
                        Report-BoltDependency 'OK' "Visual Studio build environment: $visualStudio"
                    } else {
                        Report-BoltDependency 'WARN' 'Visual Studio environment setup failed; checking the current toolchain environment.'
                    }
                } catch { Report-BoltDependency 'WARN' ('Visual Studio environment setup failed: ' + $_.Exception.Message) }
            }
        }

        if ($BuildRequired) {
            $cmake = Resolve-BoltDependencyCommand 'cmake.exe'
            if ($cmake) {
                $version = Invoke-BoltDependencyProcess $cmake @('--version')
                if ($version.ExitCode -eq 0 -and $version.StdOut -match 'cmake version (\d+\.\d+(?:\.\d+)?)' -and [version]$matches[1] -ge [version]'3.21') {
                    $result.CMake = $cmake
                    Report-BoltDependency 'OK' "CMake $($matches[1]): $cmake"
                }
            }
            if (-not $result.CMake) { Missing-BoltDependency 'Source build requires CMake 3.21 or newer on PATH (https://cmake.org/download/).' }
            $pythonCandidates = New-Object 'System.Collections.Generic.List[string]'
            foreach ($name in @('python.exe', 'python3.exe')) { $candidate = Resolve-BoltDependencyCommand $name; if ($candidate) { [void]$pythonCandidates.Add($candidate) } }
            $launcher = Resolve-BoltDependencyCommand 'py.exe'
            if ($launcher) {
                $pythonPath = Invoke-BoltDependencyProcess $launcher @('-3', '-c', 'import sys; print(sys.executable)') -TimeoutSeconds 10
                if ($pythonPath.ExitCode -eq 0) { $candidate = Resolve-BoltDependencyCommand $pythonPath.StdOut.Trim(); if ($candidate) { [void]$pythonCandidates.Add($candidate) } }
            }
            foreach ($candidate in $pythonCandidates) {
                $version = Invoke-BoltDependencyProcess $candidate @('-c', 'import sys; print(sys.version_info.major); print(sys.executable)') -TimeoutSeconds 10
                if ($version.ExitCode -eq 0 -and $version.StdOut -match '^3[\r\n]') { $result.Python = $candidate; break }
            }
            if ($result.Python) { Report-BoltDependency 'OK' "Python 3: $($result.Python)" }
            else { Missing-BoltDependency 'Source build requires a working Python 3 interpreter on PATH (https://www.python.org/downloads/windows/).' }
        }

        if ($linkerOverride) {
            $candidate = Resolve-BoltDependencyCommand $linkerOverride
            if (-not $candidate) { Missing-BoltDependency "Explicit linker override does not name an existing executable: $linkerOverride" }
        } else {
            $candidate = Resolve-BoltDependencyCommand 'lld-link.exe'
            if (-not $candidate) { $candidate = Resolve-BoltDependencyCommand 'link.exe' }
        }
        if ($candidate) {
            $linkProbe = Invoke-BoltDependencyProcess $candidate @('/?') -TimeoutSeconds 15
            $linkText = $linkProbe.StdOut + $linkProbe.StdErr
            # MSVC LINK intentionally exits 1100 when displaying /? help.
            if ($linkProbe.ExitCode -in @(0, 1100) -and $linkText -match '(?i)(Microsoft.*Linker|LLVM|LLD|lld-link|/OUT:|/out:)') {
                $result.Linker = $candidate
                [Environment]::SetEnvironmentVariable('BOLT_LINKER', $candidate, 'Process')
                Report-BoltDependency 'OK' "Native linker: $candidate"
            } else { Missing-BoltDependency "Native linker cannot run or is not an MSVC-compatible linker: $candidate. $(Short-BoltDependencyError $linkProbe)" }
        } elseif (-not $linkerOverride) {
            Missing-BoltDependency 'Native AOT requires link.exe (Visual Studio C++ Build Tools) or lld-link.exe. Set BOLT_LINKER to its full path.'
        }

        $probeRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
        $probeDirectory = Join-Path $probeRoot ('bolt-dependency-probe-' + [Guid]::NewGuid().ToString('N'))
        [void](New-Item -ItemType Directory -Path $probeDirectory)
        $cxx = Resolve-BoltDependencyCommand 'cl.exe'
        if (-not $cxx) { $cxx = Resolve-BoltDependencyCommand 'clang-cl.exe' }
        $cxxFailure = 'A C++20 toolchain and Windows SDK are unavailable.'
        if ($cxx) {
            $cpp = Join-Path $probeDirectory 'toolchain.cpp'
            $exe = Join-Path $probeDirectory 'toolchain.exe'
            $obj = Join-Path $probeDirectory 'toolchain.obj'
            @('#include <windows.h>', '#include <concepts>', 'static_assert(std::same_as<int, int>);', 'int main() { return GetCurrentProcessId() == 0 ? 1 : 0; }') | Set-Content -LiteralPath $cpp -Encoding ASCII
            $compile = Invoke-BoltDependencyProcess $cxx @('/nologo', '/std:c++20', '/EHsc', '/MD', $cpp, ('/Fe' + $exe), ('/Fo' + $obj)) -WorkingDirectory $probeDirectory -TimeoutSeconds 60
            if ($compile.ExitCode -eq 0 -and (Test-Path -LiteralPath $exe -PathType Leaf)) {
                $run = Invoke-BoltDependencyProcess $exe -WorkingDirectory $probeDirectory
                $archiver = Resolve-BoltDependencyCommand 'lib.exe'
                if (-not $archiver) { $archiver = Resolve-BoltDependencyCommand 'llvm-lib.exe' }
                if ($run.ExitCode -eq 0 -and $archiver) {
                    $archive = Join-Path $probeDirectory 'toolchain.lib'
                    $pack = Invoke-BoltDependencyProcess $archiver @('/nologo', ('/OUT:' + $archive), $obj) -WorkingDirectory $probeDirectory
                    if ($pack.ExitCode -eq 0 -and (Test-Path -LiteralPath $archive -PathType Leaf)) {
                        $result.Cxx = $cxx
                        Report-BoltDependency 'OK' "C++20 compiler, Windows SDK, archiver and runtime: $cxx (compiled and executed)"
                    } else { $cxxFailure = 'C++ archiver probe failed: ' + (Short-BoltDependencyError $pack) }
                } elseif ($run.ExitCode -ne 0) { $cxxFailure = 'C++ runtime probe failed: ' + (Short-BoltDependencyError $run) }
                else { $cxxFailure = 'C++ compiler works, but lib.exe/llvm-lib.exe is missing.' }
            } else { $cxxFailure = 'C++20/Windows SDK compile-and-link probe failed: ' + (Short-BoltDependencyError $compile) }
        }
        if (-not $result.Cxx) {
            $message = $cxxFailure + ' Install Visual Studio Build Tools with Desktop development with C++ and a Windows SDK.'
            if ($BuildRequired) { Missing-BoltDependency $message }
            else { Report-BoltDependency 'WARN' ($message + ' C++ fallback, FFI and separate library builds will be unavailable.') }
        }

        $bolt = ''
        foreach ($directory in @($SourceBin, $(if ($Root) { Join-Path $Root 'build\install-windows\bin\Release' }), $(if ($Root) { Join-Path $Root 'build\install-windows\bin' }), $(if ($Prefix) { Join-Path $Prefix 'libexec' }))) {
            if ($directory) { $candidate = Join-Path $directory 'bolt.exe'; if (Test-Path -LiteralPath $candidate -PathType Leaf) { $bolt = [IO.Path]::GetFullPath($candidate); break } }
        }
        if ($bolt) {
            $version = Invoke-BoltDependencyProcess $bolt @('version')
            if ($version.ExitCode -eq 0 -and $version.StdOut -match 'Bolt \d+\.') {
                $result.CompilerVerified = $true
                Report-BoltDependency 'OK' "Bolt compiler and its VC++ runtime DLLs load: $bolt"
            } else { Missing-BoltDependency ('Bolt cannot run. Install the matching Microsoft Visual C++ Redistributable and verify the compiler architecture. ' + (Short-BoltDependencyError $version)) }
        } elseif ($BuildRequired) { Report-BoltDependency 'WARN' 'Compiler/runtime execution will be checked after the source build.' }
        else { Missing-BoltDependency 'No bolt.exe is available to verify the installed compiler and native dependencies.' }

        $llvmCandidates = @()
        if ($llvmOverride) {
            $llvmPath = [Environment]::ExpandEnvironmentVariables($llvmOverride.Trim().Trim('"'))
            if (Test-Path -LiteralPath $llvmPath -PathType Leaf) { $llvmCandidates = @([IO.Path]::GetFullPath($llvmPath)) }
            else { Missing-BoltDependency "Explicit LLVM override does not name an existing shared library: $llvmOverride" }
        } else { $llvmCandidates = @(Get-BoltDependencyLlvmCandidates $SourceBin $Prefix $visualStudio) }
        $llvmFailures = New-Object 'System.Collections.Generic.List[string]'
        foreach ($candidate in $llvmCandidates) {
            if (-not $result.CompilerVerified) {
                if ($BuildRequired -and -not $bolt) {
                    $result.LlvmLibrary = $candidate
                    Report-BoltDependency 'WARN' "LLVM candidate will be verified after building Bolt: $candidate"
                }
                break
            }
            $previousLLVM = [Environment]::GetEnvironmentVariable('BOLT_LLVM_LIBRARY', 'Process')
            try {
                [Environment]::SetEnvironmentVariable('BOLT_LLVM_LIBRARY', $candidate, 'Process')
                $listing = Invoke-BoltDependencyProcess $bolt @('backends') -TimeoutSeconds 20
                if ($listing.ExitCode -ne 0 -or $listing.StdOut -notmatch '(?m)^LLVM:\s+(\d+)\.\d+(?:\.\d+)?\s+\(AOT/JIT\)' -or [int]$matches[1] -lt 19) {
                    [void]$llvmFailures.Add("${candidate}: " + (Short-BoltDependencyError $listing))
                    continue
                }
                $llvmVersion = $matches[1]
                $jit = Invoke-BoltDependencyProcess $bolt @('exec', 'Print(42);', '--jit', '--backend', 'llvm', '--require-native', '--llvm', $candidate) -WorkingDirectory $probeDirectory -TimeoutSeconds 45
                if ($jit.ExitCode -eq 0 -and $jit.StdOut.Trim() -eq '42') {
                    $result.LlvmLibrary = $candidate
                    Report-BoltDependency 'OK' "LLVM $llvmVersion shared library and host MCJIT: $candidate (executed Bolt code)"
                    break
                }
                [void]$llvmFailures.Add("${candidate}: " + (Short-BoltDependencyError $jit))
            } finally {
                if (-not $result.LlvmLibrary) { [Environment]::SetEnvironmentVariable('BOLT_LLVM_LIBRARY', $previousLLVM, 'Process') }
            }
        }
        if (-not $result.LlvmLibrary -and (-not $llvmOverride -or $llvmCandidates.Count -gt 0)) {
            $message = 'A working LLVM 19+ shared C API library with host target and MCJIT is required. Set BOLT_LLVM_LIBRARY to LLVM-C.dll/libLLVM.dll (clang.exe alone is insufficient).'
            if ($llvmFailures.Count) { $message += ' Probe: ' + ($llvmFailures -join '; ') }
            if ($AllowMissingLLVM -and -not $llvmOverride) { Report-BoltDependency 'WARN' ($message + ' Reduced installation explicitly allowed; LLVM Release/JIT are unavailable.') }
            else { Missing-BoltDependency $message }
        }
    } catch { Missing-BoltDependency ('Dependency verification failed: ' + $_.Exception.Message) }
    finally {
        if ($probeDirectory -and (Test-Path -LiteralPath $probeDirectory)) {
            try {
                $resolved = [IO.Path]::GetFullPath($probeDirectory)
                $expected = $probeRoot + '\'
                $item = Get-Item -LiteralPath $resolved -Force
                if (-not $resolved.StartsWith($expected, [StringComparison]::OrdinalIgnoreCase) -or
                    [IO.Path]::GetFileName($resolved) -notlike 'bolt-dependency-probe-*' -or
                    ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe dependency-probe cleanup path.' }
                Remove-Item -LiteralPath $resolved -Recurse -Force
            } catch { Report-BoltDependency 'WARN' ('Could not remove dependency probe directory: ' + $_.Exception.Message) }
        }
    }
    $result.Errors = [string[]]$errors.ToArray()
    $result.Success = $errors.Count -eq 0
    return [pscustomobject]$result
}
