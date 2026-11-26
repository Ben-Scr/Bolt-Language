# Restore only a file icon still owned by Bolt; never change the default application.
function Restore-BoltFileIcon($Registration) {
    if (-not $Registration) { return }
        $extension = $Registration.extension
        if (-not $extension) { $extension = '.blt' } # Manifests through 0.8 recorded .blt.
        if ($extension -notin @('.blt', '.bolt')) { throw 'Invalid Bolt icon extension.' }
        $extensionKeyPath = 'Software\Classes\' + $extension
        $iconKeyPath = $extensionKeyPath + '\DefaultIcon'
        $iconKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($iconKeyPath, $true)
        $removeIconKey = $false
        try {
            if ($iconKey -and ($iconKey.GetValueNames() -contains '') -and
                $iconKey.GetValueKind('') -eq [Microsoft.Win32.RegistryValueKind]::String -and
                $iconKey.GetValue('') -eq $Registration.value) {
                if ($Registration.previousValueExisted) {
                    $kind = [Microsoft.Win32.RegistryValueKind]([Enum]::Parse([Microsoft.Win32.RegistryValueKind], $Registration.previousKind))
                    $value = $Registration.previousValue
                    # JSON preserves data but not all registry value CLR types.
                    switch ($kind) {
                        'Binary' { $value = [byte[]]$value }
                        'None' { $value = [byte[]]$value }
                        'DWord' { $value = [int]$value }
                        'QWord' { $value = [long]$value }
                        'MultiString' { $value = [string[]]$value }
                        default { $value = [string]$value }
                    }
                    $iconKey.SetValue('', $value, $kind)
                } else { $iconKey.DeleteValue('', $false) }
                $removeIconKey = $Registration.keyCreated -and $iconKey.ValueCount -eq 0 -and $iconKey.SubKeyCount -eq 0
            }
        } finally { if ($iconKey) { $iconKey.Dispose() } }
        if ($removeIconKey) { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($iconKeyPath, $false) }
        if ($Registration.extensionKeyCreated) {
            $extensionKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($extensionKeyPath)
            try { $removeExtensionKey = $extensionKey -and $extensionKey.ValueCount -eq 0 -and $extensionKey.SubKeyCount -eq 0 }
            finally { if ($extensionKey) { $extensionKey.Dispose() } }
            if ($removeExtensionKey) { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($extensionKeyPath, $false) }
        }
        if (-not ('BoltIconNotifications' -as [type])) {
            Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class BoltIconNotifications { [DllImport("shell32.dll")] public static extern void SHChangeNotify(uint e, uint f, IntPtr a, IntPtr b); }'
        }
        [BoltIconNotifications]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
}
