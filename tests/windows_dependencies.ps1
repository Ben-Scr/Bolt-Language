#requires -Version 5.1
<#
Deterministic Windows installer dependency checks; no Pester or third-party tools.
Run: powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/windows_dependencies.ps1
Optional: -Bolt build/install-windows/bin/Release/bolt.exe
The optional compiler check invokes only --version/backends and does not install anything.
#>
[CmdletBinding()]
param([string]$Bolt = '')
$ErrorActionPreference = 'Stop'
$script:Passed = 0
$script:Failed = 0
$script:Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:Helper = Join-Path $script:Root 'tools\windows-dependencies.ps1'
$script:Fixture = Join-Path ([IO.Path]::GetTempPath()) ('bolt-dependency-tests-' + [Guid]::NewGuid().ToString('N'))

function Assert-True($Value, [string]$Message) {
    if (-not $Value) { throw $Message }
}
function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -cne $Expected) { throw "$Message (expected '$Expected', got '$Actual')" }
}
function Test-Case([string]$Name, [scriptblock]$Body) {
    try {
        & $Body
        $script:Passed++
        Write-Host "PASS $Name"
    } catch {
        $script:Failed++
        Write-Host "FAIL $Name`: $($_.Exception.Message)"
        Write-Host $_.ScriptStackTrace
    }
}
function Get-UserState {
    $values = [Environment]::GetEnvironmentVariables('User')
    $environment = @($values.Keys | Sort-Object | ForEach-Object { "$_=$($values[$_])" }) -join "`n"
    $compiler = Get-ItemProperty -LiteralPath 'HKCU:\Software\BoltLanguage' -ErrorAction SilentlyContinue
    [pscustomobject]@{ Environment = $environment; Compiler = $compiler.CompilerPath; CompilerKey = [bool]$compiler }
}
function Assert-UserState($Before) {
    $after = Get-UserState
    Assert-True ($after.Environment -ceq $Before.Environment) 'User environment changed'
    Assert-True ($after.Compiler -ceq $Before.Compiler) 'Bolt registry registration changed'
    Assert-True ($after.CompilerKey -eq $Before.CompilerKey) 'Bolt registry key presence changed'
}
function Test-Syntax([string]$Path) {
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    Assert-True ($errors.Count -eq 0) (($errors | ForEach-Object Message) -join '; ')
}

try {
    New-Item -ItemType Directory -Path $script:Fixture -Force | Out-Null
    Test-Case 'Dependency helper PowerShell syntax' { Test-Syntax $script:Helper }
    Test-Case 'Installer PowerShell syntax' { Test-Syntax (Join-Path $script:Root 'tools\install.ps1') }
    . $script:Helper

    Test-Case 'Child tool environment canonicalizes uppercase PATH without changing values' {
        # Use an isolated child so the uppercase spelling is guaranteed without
        # modifying this test process or depending on how it was launched.
        $check = @'
$ErrorActionPreference = 'Stop'
. '__HELPER__'
$before = [Environment]::GetEnvironmentVariables('Process')
$inheritedNames = @($before.Keys | Where-Object { $_ -ieq 'PATH' })
if ($inheritedNames.Count -ne 1 -or $inheritedNames[0] -cne 'PATH') {
    throw 'Regression fixture did not inherit exactly uppercase PATH.'
}
$start = New-Object Diagnostics.ProcessStartInfo
Initialize-BoltDependencyProcessEnvironment $start
$pathNames = @($start.EnvironmentVariables.Keys | Where-Object { $_ -ieq 'PATH' })
if ($pathNames.Count -ne 1 -or $pathNames[0] -cne 'Path') {
    throw 'Tool environment must contain exactly one variable spelled Path.'
}
if ($start.EnvironmentVariables.Count -ne $before.Count) {
    throw 'Tool environment unexpectedly added or removed a variable.'
}
foreach ($entry in $before.GetEnumerator()) {
    if ($start.EnvironmentVariables[$entry.Key] -cne $entry.Value) {
        throw 'Tool environment changed a captured value.'
    }
}
Write-Output 'Environment preserved and Path canonicalized.'
'@
        $check = $check.Replace('__HELPER__', $script:Helper.Replace("'", "''"))
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = Join-Path $PSHOME 'powershell.exe'
        if (-not (Test-Path -LiteralPath $start.FileName)) { $start.FileName = (Get-Command powershell.exe -ErrorAction Stop).Source }
        $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($check))
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $pathValue = [Environment]::GetEnvironmentVariable('Path', 'Process')
        $start.EnvironmentVariables.Remove('Path')
        $start.EnvironmentVariables['PATH'] = $pathValue
        $process = New-Object Diagnostics.Process
        try {
            $process.StartInfo = $start
            Assert-True ($process.Start()) 'Could not launch environment regression fixture'
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(15000)) {
                $process.Kill()
                throw 'Environment regression fixture timed out'
            }
            Assert-Equal $process.ExitCode 0 ($stderr.GetAwaiter().GetResult())
            Assert-True ($stdout.GetAwaiter().GetResult().Trim() -eq 'Environment preserved and Path canonicalized.') 'Environment fixture did not complete its assertions'
        } finally { $process.Dispose() }
    }

    $script:DependencyBin = Join-Path $script:Fixture 'dependencies'
    $script:EmptyBin = Join-Path $script:Fixture 'empty'
    New-Item -ItemType Directory -Path $script:DependencyBin, $script:EmptyBin -Force | Out-Null
    foreach ($name in @('bolt.exe', 'link.exe', 'cl.exe', 'clang.exe', 'lib.exe', 'cmake.exe', 'python.exe', 'LLVM-C.dll', 'libLLVM.dll')) {
        [IO.File]::WriteAllText((Join-Path $script:DependencyBin $name), 'fixture; native execution is mocked')
    }
    $script:LlvmFixture = Join-Path $script:DependencyBin 'LLVM-C.dll'
    $script:SecondLlvmFixture = Join-Path $script:DependencyBin 'libLLVM.dll'
    $script:PreviousProcessEnvironment = @{}
    foreach ($name in @('BOLT_LINKER', 'BOLT_LLVM_LIBRARY', 'VSINSTALLDIR')) {
        $script:PreviousProcessEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    function Reset-Scenario {
        $script:Scenario = @{
            Tools = @{}; Environment = @{}; Candidates = @($script:LlvmFixture);
            Listings = @{}; Listing = 'LLVM: 19.1.7 (AOT/JIT)'; ListingExit = 0;
            JitExit = 0; JitOutput = '42'; VersionExit = 0; LinkerExit = 0;
            LinkerOutput = 'Microsoft Incremental Linker /OUT:filename';
            CMakeVersion = '3.21.0'; PythonMajor = 3; CxxExit = 0; ArchiveExit = 0;
            RuntimeExit = 0; NoCompiler = $false; BuildEnvironment = '';
            Calls = New-Object 'System.Collections.Generic.List[object]'
        }
        foreach ($name in @('link.exe', 'cl.exe', 'clang.exe', 'lib.exe', 'cmake.exe', 'python.exe')) {
            $script:Scenario.Tools[$name] = Join-Path $script:DependencyBin $name
        }
    }
    function Get-BoltDependencyEnvironmentValue([string]$Name, [string]$Scope) {
        return $script:Scenario.Environment["$Scope/$Name"]
    }
    function Resolve-BoltDependencyCommand([string]$Name) {
        if ($script:Scenario.Tools.ContainsKey($Name)) { return $script:Scenario.Tools[$Name] }
        if ($script:Scenario.Tools.Values -contains $Name) { return $Name }
        return ''
    }
    function Get-BoltDependencyLlvmCandidates([string]$SourceBin, [string]$Prefix, [string]$VisualStudio) {
        return $script:Scenario.Candidates
    }
    function Invoke-BoltDependencyProcess {
        param([string]$FilePath, [string[]]$Arguments = @(), [string]$WorkingDirectory = '', [int]$TimeoutSeconds = 30, [string]$CommandLine = '')
        [void]$script:Scenario.Calls.Add([pscustomobject]@{ FilePath=$FilePath; Arguments=$Arguments; CommandLine=$CommandLine })
        $answer = [pscustomobject]@{ ExitCode=0; StdOut=''; StdErr=''; TimedOut=$false }
        switch ([IO.Path]::GetFileName($FilePath).ToLowerInvariant()) {
            'vswhere.exe' { return $answer }
            'cmd.exe' { $answer.StdOut = $script:Scenario.BuildEnvironment; return $answer }
            'link.exe' {
                $answer.ExitCode = $script:Scenario.LinkerExit
                $answer.StdOut = $script:Scenario.LinkerOutput
                return $answer
            }
            'cmake.exe' { $answer.StdOut = 'cmake version ' + $script:Scenario.CMakeVersion; return $answer }
            'python.exe' { $answer.StdOut = "$($script:Scenario.PythonMajor)`n$FilePath`n"; return $answer }
            'cl.exe' {
                $answer.ExitCode = $script:Scenario.CxxExit
                if ($answer.ExitCode -eq 0) {
                    foreach ($argument in $Arguments) {
                        if ($argument -match '^/F[eo](.+)$') { [IO.File]::WriteAllText($matches[1], 'compiled fixture') }
                    }
                } else { $answer.StdErr = 'Windows SDK header unavailable' }
                return $answer
            }
            'lib.exe' {
                $answer.ExitCode = $script:Scenario.ArchiveExit
                if ($answer.ExitCode -eq 0) {
                    foreach ($argument in $Arguments) {
                        if ($argument -match '^/OUT:(.+)$') { [IO.File]::WriteAllText($matches[1], 'archive fixture') }
                    }
                }
                return $answer
            }
            'toolchain.exe' { $answer.ExitCode = $script:Scenario.RuntimeExit; return $answer }
            'bolt.exe' {
                switch ($Arguments[0]) {
                    'version' {
                        $answer.ExitCode = $script:Scenario.VersionExit
                        $answer.StdOut = if ($answer.ExitCode -eq 0) { 'Bolt 0.15.0 (direct native backends + JIT)' } else { '' }
                        $answer.StdErr = if ($answer.ExitCode -ne 0) { 'The specified module could not be found.' } else { '' }
                    }
                    'backends' {
                        $answer.ExitCode = $script:Scenario.ListingExit
                        $candidate = [Environment]::GetEnvironmentVariable('BOLT_LLVM_LIBRARY', 'Process')
                        $answer.StdOut = if ($script:Scenario.Listings.ContainsKey($candidate)) { $script:Scenario.Listings[$candidate] } else { $script:Scenario.Listing }
                    }
                    'exec' {
                        Assert-True ($Arguments -contains '--jit') 'LLVM smoke did not request JIT'
                        Assert-True ($Arguments -contains '--require-native') 'LLVM smoke permits C++ fallback'
                        $libraryIndex = [Array]::IndexOf($Arguments, '--llvm')
                        Assert-True ($libraryIndex -ge 0) 'LLVM smoke did not pin the tested library'
                        Assert-Equal $Arguments[$libraryIndex + 1] ([Environment]::GetEnvironmentVariable('BOLT_LLVM_LIBRARY', 'Process')) 'Smoke used a different LLVM candidate'
                        $answer.ExitCode = $script:Scenario.JitExit
                        $answer.StdOut = $script:Scenario.JitOutput
                    }
                    default { throw "Unexpected Bolt probe: $($Arguments -join ' ')" }
                }
                return $answer
            }
            default { throw "Unexpected native probe: $FilePath $($Arguments -join ' ')" }
        }
    }
    function Invoke-MockedDependencies([bool]$BuildRequired = $false, [switch]$AllowMissingLLVM, [string]$LlvmLibrary = '', [string]$Linker = '') {
        $source = if ($script:Scenario.NoCompiler) { $script:EmptyBin } else { $script:DependencyBin }
        Test-BoltWindowsDependencies -Root $script:Fixture -SourceBin $source -Prefix (Join-Path $script:Fixture 'prefix') `
            -BuildRequired $BuildRequired -AllowMissingLLVM:$AllowMissingLLVM -LlvmLibrary $LlvmLibrary -Linker $Linker 6>$null
    }
    function Assert-DependencyFailure($Result, [string]$Pattern) {
        Assert-True (-not $Result.Success) 'Dependency failure unexpectedly succeeded'
        Assert-True (($Result.Errors -join '; ') -match $Pattern) "Missing actionable diagnostic '$Pattern': $($Result.Errors -join '; ')"
    }
    Test-Case 'LLVM 19 shared library requires an actual native JIT execution' {
        Reset-Scenario
        $result = Invoke-MockedDependencies
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-Equal $result.LlvmLibrary $script:LlvmFixture 'Verified library was not selected'
        Assert-Equal @($script:Scenario.Calls | Where-Object { $_.Arguments -contains 'exec' }).Count 1 'No JIT execution was performed'
    }
    Test-Case 'Visual Studio setup preserves PowerShell module discovery and installer hashing' {
        Reset-Scenario
        $visualStudio = Join-Path $script:Fixture 'visual-studio'
        $developerTools = Join-Path $visualStudio 'Common7\Tools'
        New-Item -ItemType Directory -Path $developerTools -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $developerTools 'VsDevCmd.bat'), 'fixture; execution is mocked')
        $protectedNames = @('PSModulePath', 'PSModuleAnalysisCachePath', 'PSExecutionPolicyPreference', 'PSHOME')
        $savedEnvironment = @{}
        foreach ($name in ($protectedNames + @('VSINSTALLDIR', 'VSCMD_VER'))) {
            $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        }
        try {
            [Environment]::SetEnvironmentVariable('VSINSTALLDIR', $visualStudio, 'Process')
            $script:Scenario.BuildEnvironment = (@($protectedNames | ForEach-Object { $_.ToUpperInvariant() + '=invalid-module-environment-fixture' }) + 'VSCMD_VER=verified-build-environment-fixture') -join "`r`n"
            $result = Invoke-MockedDependencies
            Assert-True $result.Success ($result.Errors -join '; ')
            foreach ($name in $protectedNames) {
                Assert-True ([Environment]::GetEnvironmentVariable($name, 'Process') -ceq $savedEnvironment[$name]) "Visual Studio setup changed protected PowerShell variable $name"
            }
            Assert-Equal ([Environment]::GetEnvironmentVariable('VSCMD_VER', 'Process')) 'verified-build-environment-fixture' 'Fixture did not exercise Visual Studio environment import'
            $hash = Get-FileHash -LiteralPath (Join-Path $script:Root 'LICENSE') -Algorithm SHA256
            Assert-True ($hash.Hash -match '^[A-Fa-f0-9]{64}$') 'Installer file hashing no longer works after environment import'
        } finally {
            foreach ($name in $savedEnvironment.Keys) {
                [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
            }
        }
    }
    Test-Case 'clang.exe alone does not satisfy the LLVM shared library dependency' {
        Reset-Scenario
        $script:Scenario.Candidates = @()
        Assert-DependencyFailure (Invoke-MockedDependencies) 'shared.*library.*clang.exe alone'
    }
    Test-Case 'backends exit zero with LLVM unavailable is a failed dependency' {
        Reset-Scenario
        $script:Scenario.Listing = 'LLVM: not available: LLVM shared library not found.'
        Assert-DependencyFailure (Invoke-MockedDependencies) 'LLVM.*not available'
        Assert-Equal @($script:Scenario.Calls | Where-Object { $_.Arguments -contains 'exec' }).Count 0 'Unavailable LLVM was executed'
    }
    Test-Case 'LLVM 18 is rejected despite a successful backend listing' {
        Reset-Scenario
        $script:Scenario.Listing = 'LLVM: 18.1.8 (AOT/JIT)'
        Assert-DependencyFailure (Invoke-MockedDependencies) 'LLVM 19\+'
    }
    Test-Case 'LLVM version discovery alone does not hide failed native execution' {
        Reset-Scenario
        $script:Scenario.JitExit = 1
        $script:Scenario.JitOutput = 'missing MCJIT symbol'
        Assert-DependencyFailure (Invoke-MockedDependencies) 'MCJIT'
    }
    Test-Case 'LLVM native execution must produce the expected result' {
        Reset-Scenario
        $script:Scenario.JitOutput = 'unrelated output'
        Assert-DependencyFailure (Invoke-MockedDependencies) 'unrelated output'
    }
    Test-Case 'A later compatible LLVM candidate can replace an incompatible automatic candidate' {
        Reset-Scenario
        $script:Scenario.Candidates = @($script:LlvmFixture, $script:SecondLlvmFixture)
        $script:Scenario.Listings[$script:LlvmFixture] = 'LLVM: 18.1.8 (AOT/JIT)'
        $result = Invoke-MockedDependencies
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-Equal $result.LlvmLibrary $script:SecondLlvmFixture 'Did not select the usable candidate'
    }
    Test-Case 'Explicit nonexistent LLVM override cannot silently use an automatic candidate' {
        Reset-Scenario
        Assert-DependencyFailure (Invoke-MockedDependencies -LlvmLibrary (Join-Path $script:Fixture 'absent.dll')) 'Explicit LLVM override'
    }
    Test-Case 'Explicit incompatible LLVM override cannot silently use another candidate' {
        Reset-Scenario
        $script:Scenario.Candidates = @($script:SecondLlvmFixture)
        $script:Scenario.Listings[$script:LlvmFixture] = 'LLVM: 18.1.8 (AOT/JIT)'
        Assert-DependencyFailure (Invoke-MockedDependencies -LlvmLibrary $script:LlvmFixture) 'LLVM 19\+'
    }
    Test-Case 'AllowMissingLLVM enables reduced mode only when no explicit override is broken' {
        Reset-Scenario
        $script:Scenario.Candidates = @()
        $result = Invoke-MockedDependencies -AllowMissingLLVM
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-Equal $result.LlvmLibrary '' 'Reduced mode invented an LLVM library'
        Assert-DependencyFailure (Invoke-MockedDependencies -AllowMissingLLVM -LlvmLibrary (Join-Path $script:Fixture 'absent.dll')) 'Explicit LLVM override'
    }
    Test-Case 'Missing native linker remains fatal in explicitly reduced mode' {
        Reset-Scenario
        [void]$script:Scenario.Tools.Remove('link.exe')
        $script:Scenario.Candidates = @()
        Assert-DependencyFailure (Invoke-MockedDependencies -AllowMissingLLVM) 'Native AOT requires'
    }
    Test-Case 'Explicit nonexistent linker override cannot silently use a discovered linker' {
        Reset-Scenario
        Assert-DependencyFailure (Invoke-MockedDependencies -Linker (Join-Path $script:Fixture 'absent.exe')) 'Explicit linker override'
    }
    Test-Case 'A wrong executable named link.exe is not accepted as a linker' {
        Reset-Scenario
        $script:Scenario.LinkerOutput = 'GNU coreutils link: create a hard link'
        Assert-DependencyFailure (Invoke-MockedDependencies) 'not an MSVC-compatible linker'
    }
    Test-Case 'MSVC linker help exit 1100 is accepted when its identity is verified' {
        Reset-Scenario
        $script:Scenario.LinkerExit = 1100
        $result = Invoke-MockedDependencies
        Assert-True $result.Success ($result.Errors -join '; ')
    }
    Test-Case 'Source build accepts CMake 3.21, Python 3, C++20, SDK and archiver' {
        Reset-Scenario
        $result = Invoke-MockedDependencies -BuildRequired $true
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-True ($result.CMake -and $result.Python -and $result.Cxx) 'Source build requirements were not verified'
    }
    Test-Case 'Source build rejects an older CMake' {
        Reset-Scenario
        $script:Scenario.CMakeVersion = '3.20.9'
        Assert-DependencyFailure (Invoke-MockedDependencies -BuildRequired $true) 'CMake 3.21'
    }
    Test-Case 'Source build rejects a Python 2 interpreter' {
        Reset-Scenario
        $script:Scenario.PythonMajor = 2
        Assert-DependencyFailure (Invoke-MockedDependencies -BuildRequired $true) 'Python 3'
    }
    Test-Case 'A compiler executable without the Windows SDK does not satisfy a source build' {
        Reset-Scenario
        $script:Scenario.CxxExit = 1
        Assert-DependencyFailure (Invoke-MockedDependencies -BuildRequired $true) 'Windows SDK'
    }
    Test-Case 'Source build rejects a missing native library archiver' {
        Reset-Scenario
        [void]$script:Scenario.Tools.Remove('lib.exe')
        Assert-DependencyFailure (Invoke-MockedDependencies -BuildRequired $true) 'lib.exe/llvm-lib.exe is missing'
    }
    Test-Case 'Prebuilt distribution does not require CMake, Python or C++ fallback tools' {
        Reset-Scenario
        foreach ($name in @('cmake.exe', 'python.exe', 'cl.exe', 'lib.exe')) { [void]$script:Scenario.Tools.Remove($name) }
        $result = Invoke-MockedDependencies
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-Equal @($script:Scenario.Calls | Where-Object { $_.FilePath -match '(cmake|python)\.exe$' }).Count 0 'Prebuilt install probed source-only dependencies'
    }
    Test-Case 'Missing compiler runtime is reported even when compiler file exists' {
        Reset-Scenario
        $script:Scenario.VersionExit = -1073741515
        Assert-DependencyFailure (Invoke-MockedDependencies) 'Visual C\+\+ Redistributable'
    }
    Test-Case 'Fresh source build defers runtime validation until the compiler exists' {
        Reset-Scenario
        $script:Scenario.NoCompiler = $true
        $result = Invoke-MockedDependencies -BuildRequired $true
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-True (-not $result.CompilerVerified) 'Nonexistent compiler was marked verified'
    }
    Test-Case 'Prebuilt installation cannot defer verification of a missing compiler' {
        Reset-Scenario
        $script:Scenario.NoCompiler = $true
        Assert-DependencyFailure (Invoke-MockedDependencies) 'No bolt.exe is available'
    }
    Test-Case 'Stale process environment uses the current User LLVM and linker settings' {
        Reset-Scenario
        $script:Scenario.Candidates = @()
        $script:Scenario.Environment['User/BOLT_LLVM_LIBRARY'] = $script:LlvmFixture
        $script:Scenario.Environment['User/BOLT_LINKER'] = $script:Scenario.Tools['link.exe']
        $result = Invoke-MockedDependencies
        Assert-True $result.Success ($result.Errors -join '; ')
        Assert-Equal $result.LlvmLibrary $script:LlvmFixture 'User LLVM setting was not retrieved'
        Assert-Equal $result.Linker $script:Scenario.Tools['link.exe'] 'User linker setting was not retrieved'
    }
    Test-Case 'Process environment has precedence over User and Machine settings' {
        Reset-Scenario
        $script:Scenario.Environment['Process/BOLT_LLVM_LIBRARY'] = ' "process.dll" '
        $script:Scenario.Environment['User/BOLT_LLVM_LIBRARY'] = 'user.dll'
        $script:Scenario.Environment['Machine/BOLT_LLVM_LIBRARY'] = 'machine.dll'
        Assert-Equal (Get-BoltDependencyEnvironment 'BOLT_LLVM_LIBRARY') 'process.dll' 'Environment precedence or quoting changed'
    }
    Test-Case 'Machine environment is considered when Process and User settings are absent' {
        Reset-Scenario
        $script:Scenario.Environment['Machine/BOLT_LLVM_LIBRARY'] = 'machine.dll'
        Assert-Equal (Get-BoltDependencyEnvironment 'BOLT_LLVM_LIBRARY') 'machine.dll' 'Machine setting was not retrieved'
    }
    # Restore real helper functions before checking installer entry points.
    . $script:Helper
    foreach ($name in $script:PreviousProcessEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $script:PreviousProcessEnvironment[$name], 'Process')
    }

    # Installer fixtures run in a child process. A fake preflight plus mutation
    # tripwires lets us exercise successful check-only flow without installing
    # tools, touching the real registry, or relying on this machine's SDKs.
    $installerRoot = Join-Path $script:Fixture 'installer'
    $installerTools = Join-Path $installerRoot 'tools'
    $installerBin = Join-Path $installerRoot 'distribution'
    $installerBuildBin = Join-Path $installerRoot 'build\install-windows\bin\Release'
    New-Item -ItemType Directory -Path $installerTools, $installerBin, $installerBuildBin -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $script:Root 'tools\install.ps1') -Destination $installerTools
    Copy-Item -LiteralPath (Join-Path $script:Root 'tools\file-icons.ps1') -Destination $installerTools
    foreach ($name in @('bolt.exe', 'BoltCore.lib', 'BoltRuntime.dll', 'BoltRuntime.lib', 'BoltCompiler.dll', 'BoltCompiler.lib')) {
        [IO.File]::WriteAllText((Join-Path $installerBin $name), 'test fixture')
        [IO.File]::WriteAllText((Join-Path $installerBuildBin $name), 'test fixture')
    }
    @'
function Test-BoltWindowsDependencies {
    param([string]$Root, [string]$SourceBin, [string]$Prefix, [bool]$BuildRequired,
          [switch]$AllowMissingLLVM, [string]$LlvmLibrary, [string]$Linker)
    Write-Host "FIXTURE BuildRequired=$BuildRequired AllowMissingLLVM=$([bool]$AllowMissingLLVM)"
    $ok = $env:BOLT_DEPENDENCY_TEST_SUCCESS -eq '1'
    [pscustomobject]@{ Success = $ok; Errors = @('fixture dependency unavailable');
        CMake = ''; Python = ''; Cxx = ''; Linker = 'fixture-link.exe'; LlvmLibrary = ''; CompilerVerified = $true }
}
function Invoke-BoltDependencyCommand {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$WorkingDirectory, [int]$TimeoutSeconds)
    [pscustomobject]@{ ExitCode = 0; Output = $(if ($ArgumentList -contains 'compile') { '' } else { '42' }) }
}
function New-Item {
    param([string]$ItemType, [string[]]$Path, [switch]$Force)
    foreach ($entry in $Path) {
        if ($entry -notmatch '[\\/]bolt-install-probe-[a-f0-9]+$') { throw "INSTALLER MUTATION: New-Item $entry" }
    }
    Microsoft.PowerShell.Management\New-Item -ItemType $ItemType -Path $Path -Force:$Force
}
function Copy-Item { throw 'INSTALLER MUTATION: Copy-Item' }
function Set-Content {
    param([string]$LiteralPath, [string]$Encoding, [Parameter(ValueFromPipeline=$true)]$Value)
    process {
        if ($LiteralPath -notmatch '[\\/]bolt-install-probe-[a-f0-9]+[\\/]Probe\.bolt$') {
            throw "INSTALLER MUTATION: Set-Content $LiteralPath"
        }
        Microsoft.PowerShell.Management\Set-Content -LiteralPath $LiteralPath -Encoding $Encoding -Value $Value
    }
}
function New-ItemProperty { throw 'INSTALLER MUTATION: New-ItemProperty' }
function Add-Type { throw 'INSTALLER MUTATION: Add-Type' }
'@ | Set-Content -LiteralPath (Join-Path $installerTools 'windows-dependencies.ps1') -Encoding UTF8
    $script:InstallerPath = Join-Path $installerTools 'install.ps1'
    $script:InstallerBin = $installerBin
    $script:InstallerPrefix = Join-Path $script:Fixture 'never-installed'
    $script:PowerShellExe = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $script:PowerShellExe)) {
        $script:PowerShellExe = (Get-Command powershell.exe -ErrorAction Stop).Source
    }

    function Invoke-InstallerFixture([bool]$Success, [string[]]$ExtraArguments, [switch]$BuildFromSource) {
        $before = Get-UserState
        $previousSuccess = $env:BOLT_DEPENDENCY_TEST_SUCCESS
        $previousPreference = $ErrorActionPreference
        $env:BOLT_DEPENDENCY_TEST_SUCCESS = if ($Success) { '1' } else { '0' }
        try {
            $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:InstallerPath,
                '-Prefix', $script:InstallerPrefix, '-NoPath')
            if (-not $BuildFromSource) { $arguments += @('-SourceBin', $script:InstallerBin) }
            $arguments += $ExtraArguments
            $ErrorActionPreference = 'Continue'
            $output = & $script:PowerShellExe @arguments 2>&1 | Out-String
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
            $env:BOLT_DEPENDENCY_TEST_SUCCESS = $previousSuccess
            Assert-UserState $before
        }
        Assert-True (-not (Test-Path -LiteralPath $script:InstallerPrefix)) 'Check created installation prefix'
        Assert-True ($output -notmatch 'INSTALLER MUTATION') "Installer reached a mutation: $output"
        [pscustomobject]@{ ExitCode = $code; Output = $output }
    }
    Test-Case 'CheckDependencies succeeds without installation or persistent changes' {
        $result = Invoke-InstallerFixture $true @('-CheckDependencies')
        Assert-Equal $result.ExitCode 0 $result.Output
        Assert-True ($result.Output -match 'BuildRequired=False AllowMissingLLVM=False') 'Prebuilt check used wrong requirements'
    }
    Test-Case 'CheckDependencies reports missing dependencies without persistent changes' {
        $result = Invoke-InstallerFixture $false @('-CheckDependencies')
        Assert-True ($result.ExitCode -ne 0) 'Missing dependencies reported success'
    }
    Test-Case 'Default installation stops before mutation when dependencies are missing' {
        $result = Invoke-InstallerFixture $false @()
        Assert-True ($result.ExitCode -ne 0) 'Installer accepted missing dependencies'
    }
    Test-Case 'AllowMissingLLVM is explicit and reaches check-only preflight' {
        $result = Invoke-InstallerFixture $true @('-CheckDependencies', '-AllowMissingLLVM', '-NoBuild')
        Assert-Equal $result.ExitCode 0 $result.Output
        Assert-True ($result.Output -match 'AllowMissingLLVM=True') 'Explicit reduced-mode flag was lost'
    }
    Test-Case 'Source installation requests build dependencies during check-only preflight' {
        $result = Invoke-InstallerFixture $true @('-CheckDependencies') -BuildFromSource
        Assert-Equal $result.ExitCode 0 $result.Output
        Assert-True ($result.Output -match 'BuildRequired=True') 'Source build skipped its build dependencies'
    }
    Test-Case 'NoBuild skips source-build requirements during check-only preflight' {
        $result = Invoke-InstallerFixture $true @('-CheckDependencies', '-NoBuild') -BuildFromSource
        Assert-Equal $result.ExitCode 0 $result.Output
        Assert-True ($result.Output -match 'BuildRequired=False') 'NoBuild unexpectedly requested build tools'
    }
    Test-Case 'A locked managed compiler stops an upgrade before any installed file changes' {
        $upgradeRoot = Join-Path $script:Fixture 'upgrade-source'
        $upgradeTools = Join-Path $upgradeRoot 'tools'
        $upgradeBin = Join-Path $upgradeRoot 'distribution'
        $managedPrefix = Join-Path $script:Fixture 'locked-installation'
        foreach ($directory in @($upgradeTools, $upgradeBin, (Join-Path $upgradeRoot 'include\bolt'),
            (Join-Path $upgradeRoot 'docs\licenses'), (Join-Path $upgradeRoot 'stdlib'), (Join-Path $upgradeRoot 'assets'),
            (Join-Path $managedPrefix 'libexec'), (Join-Path $managedPrefix 'share'))) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        foreach ($name in @('install.ps1', 'uninstall.ps1', 'file-icons.ps1', 'bolt.cmd', 'bolt-watch.py')) {
            Copy-Item -LiteralPath (Join-Path $script:Root ('tools\' + $name)) -Destination $upgradeTools
        }
        foreach ($name in @('bolt.exe', 'BoltCore.lib', 'BoltRuntime.dll', 'BoltRuntime.lib', 'BoltCompiler.dll', 'BoltCompiler.lib')) {
            [IO.File]::WriteAllText((Join-Path $upgradeBin $name), 'new payload fixture: ' + $name)
        }
        foreach ($relative in @('LICENSE', 'README.md', 'include\bolt\Runtime.hpp', 'docs\licenses\fixture.txt', 'stdlib\Fixture.bolt', 'assets\bolt-file.ico')) {
            [IO.File]::WriteAllText((Join-Path $upgradeRoot $relative), 'new fixture: ' + $relative)
        }
        $ownedFiles = @{}
        foreach ($relative in @('libexec\bolt.exe', 'libexec\BoltRuntime.dll', 'share\README.md')) {
            $path = Join-Path $managedPrefix $relative
            [IO.File]::WriteAllText($path, 'old installed fixture: ' + $relative)
            $ownedFiles[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        }
        @{ product='bolt-user-install-v1'; version='0.15.0'; prefix=$managedPrefix; files=$ownedFiles;
            pathAdded=$false; registryOwned=$false; previousCompiler=$null; iconRegistration=$null; environment=@{} } |
            ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $managedPrefix '.bolt-install.json') -Encoding UTF8
        @'
function Test-BoltWindowsDependencies {
    [pscustomobject]@{ Success=$true; Errors=@(); CMake=''; Python=''; Cxx='';
        Linker=''; LlvmLibrary=''; CompilerVerified=$true }
}
function Invoke-BoltDependencyCommand {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$WorkingDirectory, [int]$TimeoutSeconds)
    [pscustomobject]@{ ExitCode=0; Output=$(if ($ArgumentList -contains 'compile') { '' } else { '42' }) }
}
function New-Item {
    param([string]$ItemType, [string[]]$Path, [switch]$Force)
    foreach ($entry in $Path) {
        if ($entry -match '^HK(CU|LM):') { throw 'PERSISTENT MUTATION: Registry creation' }
    }
    Microsoft.PowerShell.Management\New-Item -ItemType $ItemType -Path $Path -Force:$Force
}
function New-ItemProperty { throw 'PERSISTENT MUTATION: Registry property' }
function Add-Type { throw 'PERSISTENT MUTATION: Registration notifications' }
'@ | Set-Content -LiteralPath (Join-Path $upgradeTools 'windows-dependencies.ps1') -Encoding UTF8
        $beforeFiles = @{}
        foreach ($file in Get-ChildItem -LiteralPath $managedPrefix -Recurse -File) {
            $beforeFiles[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
        $before = Get-UserState
        $lock = [IO.File]::Open((Join-Path $managedPrefix 'libexec\bolt.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $upgradeTools 'install.ps1'),
                '-Prefix', $managedPrefix, '-SourceBin', $upgradeBin, '-NoBuild', '-NoPath')
            $result = Invoke-BoltDependencyCommand -FilePath $script:PowerShellExe -ArgumentList $arguments -TimeoutSeconds 30
            Assert-True ($result.ExitCode -ne 0) 'Upgrade unexpectedly accepted a changed locked compiler'
            Assert-True ($result.Output -match '(?s)Cannot update file in use or write-protected:.*bolt\.exe') ('Upgrade did not identify the locked compiler: ' + $result.Output)
            Assert-True ($result.Output -match 'No installed files have been changed') 'Upgrade did not fail during lock preflight'
            Assert-True ($result.Output -notmatch 'PERSISTENT MUTATION:') 'Upgrade reached persistent settings changes'
            $afterFiles = @(Get-ChildItem -LiteralPath $managedPrefix -Recurse -File)
            Assert-Equal $afterFiles.Count $beforeFiles.Count 'Upgrade added or removed installed files before checking locks'
            foreach ($file in $afterFiles) {
                Assert-True ($beforeFiles.ContainsKey($file.FullName)) 'Upgrade created an installed file before checking locks'
                Assert-True ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ceq $beforeFiles[$file.FullName]) 'Upgrade changed an installed file before checking locks'
            }
        } finally {
            $lock.Dispose()
            Assert-UserState $before
        }
    }
    Test-Case 'install.bat repairs inherited module discovery without changing its caller' {
        $entrypointRoot = Join-Path $script:Fixture 'batch-entrypoint'
        $entrypointTools = Join-Path $entrypointRoot 'tools'
        New-Item -ItemType Directory -Path $entrypointTools -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $script:Root 'install.bat') -Destination $entrypointRoot
        [IO.File]::WriteAllText((Join-Path $entrypointRoot 'hash-fixture.txt'), 'installer hashing fixture')
        @'
param([switch]$CheckDependencies)
$ErrorActionPreference = 'Stop'
if (-not $CheckDependencies) { throw 'Batch entrypoint lost its forwarded arguments.' }
$hash = Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '..\hash-fixture.txt') -Algorithm SHA256
if ($hash.Hash -notmatch '^[A-Fa-f0-9]{64}$') { throw 'Batch entrypoint could not load installer hashing.' }
$json = @{ fixture = 'module-loading' } | ConvertTo-Json -Compress
if ($json -notmatch '"fixture":"module-loading"') { throw 'Batch entrypoint could not load installer JSON serialization.' }
Write-Output 'Batch entrypoint hashing succeeded.'
'@ | Set-Content -LiteralPath (Join-Path $entrypointTools 'install.ps1') -Encoding UTF8
        @'
@echo off
call "%~dp0install.bat" -CheckDependencies
if errorlevel 1 exit /b 1
if not "%PSModulePath%"=="%BOLT_DEPENDENCY_TEST_HOSTILE_MODULEPATH%" exit /b 9
echo Caller module environment preserved.
'@ | Set-Content -LiteralPath (Join-Path $entrypointRoot 'caller.cmd') -Encoding ASCII
        $hostileModules = Join-Path $entrypointRoot 'PowerShell7-only-modules'
        New-Item -ItemType Directory -Path $hostileModules -Force | Out-Null
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
        $start.Arguments = '/d /s /c ""' + (Join-Path $entrypointRoot 'caller.cmd') + '""'
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.EnvironmentVariables['PSModulePath'] = $hostileModules
        $start.EnvironmentVariables['BOLT_DEPENDENCY_TEST_HOSTILE_MODULEPATH'] = $hostileModules
        $process = New-Object Diagnostics.Process
        $before = Get-UserState
        try {
            $process.StartInfo = $start
            Assert-True ($process.Start()) 'Could not launch batch entrypoint fixture'
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(15000)) {
                $process.Kill()
                throw 'Batch entrypoint fixture timed out'
            }
            Assert-Equal $process.ExitCode 0 ($stderr.GetAwaiter().GetResult())
            $output = $stdout.GetAwaiter().GetResult()
            Assert-True ($output -match 'Batch entrypoint hashing succeeded\.') 'Batch did not verify module loading'
            Assert-True ($output -match 'Caller module environment preserved\.') 'Batch changed its caller module environment'
        } finally {
            $process.Dispose()
            Assert-UserState $before
        }
    }

    if ($Bolt) {
        Test-Case 'Real compiler reports its executable and backend availability' {
            $resolvedBolt = [IO.Path]::GetFullPath($Bolt)
            Assert-True (Test-Path -LiteralPath $resolvedBolt) "Missing compiler: $resolvedBolt"
            $version = & $resolvedBolt --version 2>&1 | Out-String
            Assert-Equal $LASTEXITCODE 0 $version
            Assert-True ($version -match 'Bolt \d+\.\d+') 'Not a Bolt compiler'
            $backends = & $resolvedBolt backends 2>&1 | Out-String
            Assert-Equal $LASTEXITCODE 0 $backends
            Assert-True ($backends -match '(?m)^LLVM: (\d+\.|not available:)') 'Backend availability report missing'
        }
    }
} finally {
    if ($script:PreviousProcessEnvironment) {
        foreach ($name in $script:PreviousProcessEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $script:PreviousProcessEnvironment[$name], 'Process')
        }
    }
    # Remove only this test's generated directory, with an absolute containment check.
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolvedFixture = [IO.Path]::GetFullPath($script:Fixture)
    if ($resolvedFixture.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedFixture).StartsWith('bolt-dependency-tests-') -and
        (Test-Path -LiteralPath $resolvedFixture)) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
Write-Host "$script:Passed passed, $script:Failed failed"
if ($script:Failed) { exit 1 }
