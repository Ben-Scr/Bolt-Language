# Test the Windows user PATH transformation without editing the actual user registry.
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$scriptPath = Join-Path $PSScriptRoot '../tools/sdk-setup/sdk-setup.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $scriptPath),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw $errors[0] }
foreach ($name in @('Same-Path','Get-PathEdit')) {
    $node = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
    . ([scriptblock]::Create($node.Extent.Text))
}
$bin = 'C:\My SDK\bin'
$state = [pscustomobject]@{ownedPath=$false; pathWasMissing=$false}
if (Get-PathEdit ('D:\other;"C:\MY SDK\BIN\";E:\Other') $bin $true $state) { throw 'Pre-existing equivalent user PATH must be preserved' }
if (Get-PathEdit $bin $bin $false $state) { throw 'Never remove an unowned PATH entry' }
foreach ($old in @('%USERPROFILE%\Tools;;D:\Another;','',$null)) {
    $new = (Get-PathEdit $old $bin $true $state).value
    if ((Get-PathEdit $new $bin $true $state)) { throw 'Repeated setup must keep unchanged PATH' }
    $state.ownedPath = $true
    $state.pathWasMissing = $null -eq $old
    $removed = Get-PathEdit $new $bin $false $state
    if ($null -eq $old) {
        if ($removed.action -ne 'delete') { throw 'Originally absent registry value should be deleted' }
    } elseif ($removed.action -ne 'set' -or $removed.value -cne $old) { throw 'Restore raw registry value including empty or unexpanded entries' }
}
$extra = $bin + ';X:\Other;' + $bin
if ((Get-PathEdit $extra $bin $false $state).value -cne ('X:\Other;' + $bin)) { throw 'Remove only one matching occurrence' }
Write-Host 'PASS: duplicate, case, quoted, missing, empty, expanded and independently edited user PATH entries'
