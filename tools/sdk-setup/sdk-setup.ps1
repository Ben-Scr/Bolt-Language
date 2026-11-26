[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Install','Uninstall')][string]$Action,
    [string]$Prefix,
    [string]$Source = $PSScriptRoot,
    [switch]$SkipVSCode,
    [switch]$NoPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$marker = '.bolt-portable-install.json'
$product = 'bolt-portable-sdk-v1'
$extension = 'ben-schneider.bolt-language'
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Assert-NoLink([string]$Path) {
    while ($Path) {
        $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue -Force
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Junction or symbolic link in installation path: $Path"
        }
        $Path = [IO.Path]::GetDirectoryName($Path)
    }
}
function Get-DefaultPrefix([string]$Programs) {
    $primary = Join-Path $Programs 'Bolt'
    $side = Join-Path $Programs 'Bolt-SDK'
    # Prefer an existing portable install even after a legacy install was removed.
    foreach ($candidate in @($primary, $side)) {
        $path = Join-Path $candidate $marker
        if ([IO.File]::Exists($path)) {
            Assert-NoLink $path
            $info = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
            if ($info.product -eq $product -and $info.prefix -eq $candidate) { return $candidate }
        }
    }
    $legacy = Join-Path $primary '.bolt-install.json'
    if ([IO.File]::Exists($legacy)) {
        Assert-NoLink $legacy
        $info = Get-Content -Raw -LiteralPath $legacy | ConvertFrom-Json
        if ($info.product -eq 'bolt-user-install-v1' -and $info.prefix -eq $primary) {
            return $side
        }
    }
    return $primary
}
function Get-LocalFile([string]$Root, [string]$Name) {
    foreach ($part in $Name.Split('/')) {
        if (!$part -or $part -match '[\\:<>|*?"\x00-\x1f]|[ .]$|^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') {
            throw "Invalid SDK file name: $Name"
        }
    }
    $path = [IO.Path]::Combine($Root, $Name.Replace('/', [IO.Path]::DirectorySeparatorChar))
    Assert-NoLink $path
    return $path
}
function Get-Digest([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Save-State([string]$Folder, $State) {
    $file = Join-Path $Folder $marker
    $temp = Join-Path $Folder ([guid]::NewGuid().ToString('N'))
    $backup = $temp + '.bak'
    try {
        [IO.File]::WriteAllText($temp, ($State | ConvertTo-Json -Depth 6), $utf8)
        if ([IO.File]::Exists($file)) { [IO.File]::Replace($temp, $file, $backup) }
        else { [IO.File]::Move($temp, $file) }
    } finally {
        [IO.File]::Delete($temp)
        [IO.File]::Delete($backup)
    }
}
function Read-State([string]$Root) {
    $file = Get-LocalFile $Root $marker
    if (![IO.File]::Exists($file)) { throw "Unmanaged directory; choose -Prefix or move the existing installation: $Root" }
    $state = [IO.File]::ReadAllText($file) | ConvertFrom-Json
    if ($state.product -ne $product -or $state.prefix -ne $Root) { throw "Unmanaged installation: $Root" }
    foreach ($entry in $state.files) { $null = Get-LocalFile $Root $entry.name }
    return $state
}
function Get-Payload {
    $sums = Get-LocalFile $Source 'SHA256SUMS'
    if (![IO.File]::Exists($sums)) { throw 'SHA256SUMS missing. Use -Source with the extracted Windows SDK folder.' }
    $files = @{}
    foreach ($line in [IO.File]::ReadLines($sums)) {
        if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw 'Invalid SHA256SUMS entry' }
        $name = $Matches[2]
        $sha = $Matches[1]
        if ($files.ContainsKey($name) -or $name -in @($marker,'SHA256SUMS')) { throw "Duplicate or reserved entry: $name" }
        $files[$name] = @{source=(Get-LocalFile $Source $name); hash=$sha}
    }
    if (!$files.ContainsKey('bin/bolt.exe')) { throw 'Missing bin/bolt.exe. Select the Windows SDK.' }
    $files['SHA256SUMS'] = @{source=$sums;hash=(Get-Digest $sums)}
    foreach ($name in @('setup.cmd','uninstall.cmd','sdk-setup.ps1','SETUP.md')) {
        if (!$files.ContainsKey($name)) {
            $helper = Join-Path $PSScriptRoot $name
            $files[$name] = @{source=$helper; hash=(Get-Digest $helper)}
        }
    }
    return $files
}
function Copy-Unowned([string]$OldDir, [string]$NewDir, [string]$Relative, $Prior, $Files) {
    foreach ($p in Get-ChildItem -LiteralPath $OldDir -Force) {
        if ($p.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Move the junction or symbolic link before updating: $($p.FullName)" }
        $name = $Relative + $p.Name
        if ($name -eq $marker) { continue }
        $dest = Join-Path $NewDir $p.Name
        if ($p.PSIsContainer) {
            $null = [IO.Directory]::CreateDirectory($dest)
            Copy-Unowned $p.FullName $dest ($name + '/') $Prior $Files
        } elseif ($Prior.ContainsKey($name)) {
            if ((Get-Digest $p.FullName) -ne $Prior[$name]) { throw "Modified SDK file; move it before updating: $($p.FullName)" }
        } elseif ($Files.ContainsKey($name)) {
            throw "Would replace an unowned file: $($p.FullName)"
        } else {
            [IO.File]::Copy($p.FullName, $dest)
        }
    }
}
function Install-Files($State) {
    $files = Get-Payload
    $prior = @{}
    foreach ($entry in $State.files) { $prior[$entry.name] = $entry.hash }
    $work = Join-Path ([IO.Path]::GetDirectoryName($Prefix)) ('.bolt-sdk-' + [guid]::NewGuid().ToString('N'))
    $stage = Join-Path $work 'new'
    $backup = Join-Path $work 'old'
    $null = [IO.Directory]::CreateDirectory($stage)
    $committed = $false
    try {
        if ([IO.Directory]::Exists($Prefix)) { Copy-Unowned $Prefix $stage '' $prior $files }
        $records = @(foreach ($name in $files.Keys) {
            $dest = Get-LocalFile $stage $name
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest))
            [IO.File]::Copy($files[$name].source, $dest)
            if ((Get-Digest $dest) -ne $files[$name].hash) { throw "SDK checksum mismatch: $name" }
            [pscustomobject]@{name=$name; hash=$files[$name].hash}
        })
        $State.files = $records
        Save-State $stage $State
        if ([IO.Directory]::Exists($Prefix)) { [IO.Directory]::Move($Prefix, $backup) }
        try { [IO.Directory]::Move($stage, $Prefix); $committed = $true }
        catch {
            if ([IO.Directory]::Exists($backup)) { [IO.Directory]::Move($backup, $Prefix) }
            throw
        }
    } finally {
        if ($committed -or ![IO.Directory]::Exists($backup)) {
            # Fresh staging folder only. Reparse points were rejected during the copy.
            [IO.Directory]::Delete($work, $true)
        } else {
            Write-Warning "The previous installation is preserved in $backup"
        }
    }
}
function Same-Path([string]$Left, [string]$Right) {
    if (!$Left) { return $false }
    $entry = [Environment]::ExpandEnvironmentVariables($Left.Trim().Trim('"')).TrimEnd('\','/')
    return [string]::Equals($entry, $Right.TrimEnd('\','/'), [StringComparison]::OrdinalIgnoreCase)
}
function Broadcast-Path {
    if (-not ('BoltSdk.EnvironmentChange' -as [type])) {
        Add-Type -Namespace BoltSdk -Name EnvironmentChange -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr window, uint message, System.IntPtr parameter, string setting, uint flags, uint timeout, out System.IntPtr result);
'@
    }
    $result = [IntPtr]::Zero
    $null = [BoltSdk.EnvironmentChange]::SendMessageTimeout([IntPtr]65535, 26, [IntPtr]::Zero, 'Environment', 2, 2000, [ref]$result)
}
function Get-PathEdit($Value, [string]$Bin, [bool]$Adding, $State) {
    $parts = @()
    if ($null -ne $Value -and $Value -ne '') { $parts = @($Value.Split(';')) }
    if ($Adding) {
        foreach ($p in $parts) { if (Same-Path $p $Bin) { return $null } }
        return @{action='set'; value=((@($Bin) + $parts) -join ';')}
    }
    if (!$State.ownedPath) { return $null }
    # Remove only the literal entry added by this installer, once.
    $index = [array]::IndexOf($parts, $Bin)
    if ($index -lt 0) { return $null }
    $rest = @(for ($i=0; $i -lt $parts.Count; $i++) { if ($i -ne $index) { $parts[$i] } })
    if ($State.pathWasMissing -and !$rest.Count) { return @{action='delete'} }
    return @{action='set'; value=($rest -join ';')}
}
function Update-Path($State, [bool]$Adding) {
    $bin = Join-Path $Prefix 'bin'
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment')
    try {
        $value = $key.GetValue('Path',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $change = Get-PathEdit $value $bin $Adding $State
        if (!$change) { return }
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        if ($null -ne $value) { $kind = $key.GetValueKind('Path') }
        if ($Adding) {
            $State.ownedPath = $true
            $State.pathWasMissing = ($null -eq $value)
            Save-State $Prefix $State
        }
        if ($change.action -eq 'delete') { $key.DeleteValue('Path', $false) }
        else { $key.SetValue('Path', $change.value, $kind) }
    } finally { $key.Dispose() }
    try { Broadcast-Path }
    catch { Write-Warning 'User PATH saved; sign out and back in if a new terminal does not pick up the change.' }
}
function Extension-Version([string]$Code) {
    $lines = @(& $Code --list-extensions --show-versions)
    if ($LASTEXITCODE -ne 0) { throw 'VS Code could not list installed extensions.' }
    foreach ($line in $lines) {
        if ($line.Trim() -match ('^' + [regex]::Escape($extension) + '@(.+)$')) { return $Matches[1] }
    }
    return $null
}
function Update-Extension($State, [bool]$Adding) {
    if (!$Adding -and !$State.extension) { return }
    $command = Get-Command code -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $vsix = Join-Path $Prefix 'Bolt-VSCode.vsix'
    if (!$command -or ($Adding -and ![IO.File]::Exists($vsix))) {
        if ($Adding) {
            if ([IO.File]::Exists($vsix)) { Write-Host "Manual VS Code install: code --install-extension `"$vsix`"" }
            else { Write-Host 'No VSIX in this SDK; install it separately.' }
        } else { throw "VS Code CLI unavailable. Remove $extension manually or use -SkipVSCode." }
        return
    }
    $code = $command.Source
    $current = Extension-Version $code
    if ($Adding) {
        & $code --install-extension $vsix --force
        if ($LASTEXITCODE -ne 0) { throw 'VS Code extension installation failed. You can run setup again.' }
        $version = Extension-Version $code
        if (!$version) { throw 'VS Code did not confirm the installed Bolt extension.' }
        if ($State.extension -or !$current) { $State.extension = $version }
        Save-State $Prefix $State
    } elseif ($current -eq $State.extension) {
        & $code --uninstall-extension $extension
        if ($LASTEXITCODE -ne 0) { throw 'VS Code extension removal failed.' }
    } elseif ($current) { Write-Host "Preserved separately updated extension $current." }
}
function Remove-Sdk($State) {
    if (!$SkipVSCode) { Update-Extension $State $false }
    if ($State.ownedPath) { Update-Path $State $false }
    $dirs = @{}
    foreach ($e in $State.files) {
        $path = Get-LocalFile $Prefix $e.name
        if ([IO.File]::Exists($path)) {
            if ((Get-Digest $path) -eq $e.hash) { [IO.File]::Delete($path) }
            else { Write-Host "Preserved modified file: $path" }
        }
        $dir = [IO.Path]::GetDirectoryName($path)
        while ($dir -ne $Prefix) {
            $dirs[$dir] = $true
            $dir = [IO.Path]::GetDirectoryName($dir)
        }
    }
    [IO.File]::Delete((Join-Path $Prefix $marker))
    foreach ($dir in @($dirs.Keys | Sort-Object Length -Descending) + @($Prefix)) {
        if ([IO.Directory]::Exists($dir) -and !@(Get-ChildItem -LiteralPath $dir -Force).Count) { [IO.Directory]::Delete($dir) }
    }
    Write-Host 'Bolt removed. Open a new terminal.'
}
try {
    if (!$Prefix) {
        if ($Action -eq 'Uninstall' -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot $marker))) { $Prefix = $PSScriptRoot }
        else { $Prefix = Get-DefaultPrefix (Join-Path $env:LOCALAPPDATA 'Programs') }
    }
    $Prefix = [IO.Path]::GetFullPath($Prefix).TrimEnd('\','/')
    $Source = [IO.Path]::GetFullPath($Source).TrimEnd('\','/')
    Assert-NoLink $Prefix
    $sep = [IO.Path]::DirectorySeparatorChar
    if (!$Prefix -or $Prefix -match '[;\r\n]' -or !$Prefix.Contains($sep) -or $Prefix -eq $HOME -or
        ($HOME + $sep).StartsWith($Prefix + $sep,[StringComparison]::OrdinalIgnoreCase)) { throw 'Choose a separate dedicated install directory.' }
    if ($Action -eq 'Install' -and (($Source + $sep).StartsWith($Prefix + $sep,[StringComparison]::OrdinalIgnoreCase) -or
        ($Prefix + $sep).StartsWith($Source + $sep,[StringComparison]::OrdinalIgnoreCase))) { throw 'Run setup from the extracted SDK with a separate install directory.' }
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $id = [BitConverter]::ToString($hash.ComputeHash($utf8.GetBytes($Prefix.ToUpperInvariant()))) }
    finally { $hash.Dispose() }
    $mutex = New-Object Threading.Mutex($false, ('BoltSDK-' + $id))
    try {
        try { $null = $mutex.WaitOne() } catch [Threading.AbandonedMutexException] { }
        try {
            if ($Action -eq 'Install') {
                if ((Test-Path -LiteralPath $Prefix) -and @(Get-ChildItem -LiteralPath $Prefix -Force).Count) { $state = Read-State $Prefix }
                else { $state = [pscustomobject]@{product=$product; prefix=$Prefix; files=@(); ownedPath=$false; pathWasMissing=$false; extension=$null} }
                Install-Files $state
                if (!$NoPath) { Update-Path $state $true }
                Write-Host "Installed SDK: $Prefix"
                if (!$SkipVSCode) { Update-Extension $state $true }
                Write-Host 'Restart the terminal app (and VS Code), then run: bolt version'
            } elseif ([IO.Directory]::Exists($Prefix)) { Remove-Sdk (Read-State $Prefix) }
            else { Write-Host 'Bolt SDK is already removed.' }
        } finally { $mutex.ReleaseMutex() }
    } finally { $mutex.Dispose() }
} catch {
    [Console]::Error.WriteLine('Bolt setup: ' + $_.Exception.Message)
    exit 1
}
