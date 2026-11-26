# Check the default upgrade destination independently of the user's registry.
$ErrorActionPreference = 'Stop'
$marker = '.bolt-portable-install.json'
$product = 'bolt-portable-sdk-v1'
$scriptPath = Join-Path $PSScriptRoot '../tools/sdk-setup/sdk-setup.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw $errors[0] }
foreach ($name in @('Assert-NoLink','Get-DefaultPrefix')) {
    $node = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}, $true)
    . ([scriptblock]::Create($node.Extent.Text))
}
$programs = Join-Path ([IO.Path]::GetTempPath()) ('Bolt legacy test ' + [guid]::NewGuid().ToString('N'))
try {
    $primary = Join-Path $programs 'Bolt'
    $side = Join-Path $programs 'Bolt-SDK'
    if ((Get-DefaultPrefix $programs) -ne $primary) { throw 'Fresh install location changed' }
    [void][IO.Directory]::CreateDirectory($primary)
    $legacy = Join-Path $primary '.bolt-install.json'
    [IO.File]::WriteAllText($legacy, (@{product='foreign'; prefix=$primary} | ConvertTo-Json))
    if ((Get-DefaultPrefix $programs) -ne $primary) { throw 'Foreign marker was accepted' }
    [IO.File]::WriteAllText($legacy, (@{product='bolt-user-install-v1'; prefix=$primary} | ConvertTo-Json))
    if ((Get-DefaultPrefix $programs) -ne $side) { throw 'Legacy installation was not recognized' }
    [void][IO.Directory]::CreateDirectory($side)
    [IO.File]::WriteAllText((Join-Path $side $marker), (@{product=$product;prefix=$side} | ConvertTo-Json))
    [IO.File]::Delete($legacy)
    if ((Get-DefaultPrefix $programs) -ne $side) { throw 'Could not find portable SDK after legacy uninstall' }
    [IO.File]::WriteAllText((Join-Path $primary $marker), (@{product=$product;prefix=$primary} | ConvertTo-Json))
    if ((Get-DefaultPrefix $programs) -ne $primary) { throw 'Existing primary portable SDK ignored' }
    Write-Host 'PASS: fresh, foreign, legacy, repeat and primary installation lookup'
} finally {
    if ([IO.Directory]::Exists($programs)) { [IO.Directory]::Delete($programs, $true) }
}
