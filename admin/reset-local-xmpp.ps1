#requires -Version 5.1
<#
.SYNOPSIS
Preview or reset the configured AnyKeep account's local XMPP test data on Windows.
.EXAMPLE
.\admin\reset-local-xmpp.ps1
.EXAMPLE
.\admin\reset-local-xmpp.ps1 -Apply
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Apply,
    [string]$DataRoot,
    [string]$BackupRoot
)

function Get-XmppResetSettings {
    $settings = 'HKCU:\Software\R-Soft\AnyKeep'
    [PSCustomObject]@{
        Jid = [string](Get-ItemPropertyValue -LiteralPath $settings -Name 'storage.xmpppubsub.jid')
        InstanceId = [string](Get-ItemPropertyValue -LiteralPath $settings -Name 'storage.xmpppubsub.instanceId')
    }
}

function Assert-XmppResetTarget([string]$Root, [string]$Path) {
    if (-not $Path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A reset target is outside the selected data directory.'
    }
    # Check ancestors too: a junction at remote-cache/ must not redirect a move outside Root.
    $current = $Path
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to follow a linked reset path: $current"
            }
        }
        if ($current.Equals($Root, [StringComparison]::OrdinalIgnoreCase)) { break }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function Get-XmppResetPlan([string]$Root, [string]$Jid, [string]$InstanceId) {
    $account = $Jid.Trim().Split('/')[0]
    if ([string]::IsNullOrWhiteSpace($account)) { throw 'No configured XMPP account was found.' }
    $instanceGuid = [Guid]::Empty
    if (-not [Guid]::TryParse($InstanceId, [ref]$instanceGuid)) { throw 'Invalid XMPP storage instance ID.' }
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = ([BitConverter]::ToString($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($account)))).Replace('-', '').ToLowerInvariant().Substring(0, 16)
    } finally { $sha256.Dispose() }
    $state = 'xmpp-omemo-' + $hash + '.state'
    $relativePaths = @($state, ($state + '.trust'), ($state + '.iris'), ($state + '.iris.trust'))
    $cacheRoot = [IO.Path]::GetFullPath((Join-Path $rootPath 'remote-cache\xmpppubsub'))
    Assert-XmppResetTarget $rootPath $cacheRoot
    if (Test-Path -LiteralPath $cacheRoot) {
        $cachePattern = '^' + [regex]::Escape($InstanceId) + '-[0-9a-f]{16}\.cache$'
        foreach ($file in Get-ChildItem -LiteralPath $cacheRoot -File -Force) {
            if ($file.Name -match $cachePattern) { $relativePaths += 'remote-cache\xmpppubsub\' + $file.Name }
        }
    }
    $files = @(
        foreach ($relative in $relativePaths) {
            $source = [IO.Path]::GetFullPath((Join-Path $rootPath $relative))
            Assert-XmppResetTarget $rootPath $source
            if (Test-Path -LiteralPath $source) {
                if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Expected a state file: $source" }
                [PSCustomObject]@{ RelativePath = $relative; Source = $source; Length = (Get-Item -LiteralPath $source).Length }
            }
        }
    )
    [PSCustomObject]@{
        DataRoot = $rootPath
        AccountHash = $hash
        # QtKeychain's Windows backend uses key(), without a service prefix, as TargetName.
        CredentialTarget = 'xmpp-storage-master-key-v1:' + $account
        Files = $files
    }
}

function Initialize-XmppCredentialApi {
    if ('AnyKeepAdmin.WindowsCredentials' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AnyKeepAdmin {
    public static class WindowsCredentials {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct Credential {
            public uint Flags, Type;
            public IntPtr TargetName, Comment;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
            public uint BlobSize;
            public IntPtr Blob;
            public uint Persist, AttributeCount;
            public IntPtr Attributes, TargetAlias, UserName;
        }
        [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);
        [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CredDelete(string target, uint type, uint flags);
        [DllImport("advapi32.dll")]
        private static extern void CredFree(IntPtr credential);

        public static bool Exists(string target) {
            IntPtr pointer;
            if (CredRead(target, 1, 0, out pointer)) { CredFree(pointer); return true; }
            int error = Marshal.GetLastWin32Error();
            if (error == 1168) return false;
            throw new Win32Exception(error);
        }
        public static byte[] Read(string target) {
            IntPtr pointer;
            if (!CredRead(target, 1, 0, out pointer)) {
                int error = Marshal.GetLastWin32Error();
                if (error == 1168) return null;
                throw new Win32Exception(error);
            }
            try {
                Credential credential = (Credential)Marshal.PtrToStructure(pointer, typeof(Credential));
                byte[] data = new byte[credential.BlobSize];
                Marshal.Copy(credential.Blob, data, 0, data.Length);
                return data;
            } finally { CredFree(pointer); }
        }
        public static bool Delete(string target) {
            if (CredDelete(target, 1, 0)) return true;
            int error = Marshal.GetLastWin32Error();
            if (error == 1168) return false;
            throw new Win32Exception(error);
        }
    }
}
'@
}

function Test-XmppCredential([string]$Target) {
    Initialize-XmppCredentialApi
    [AnyKeepAdmin.WindowsCredentials]::Exists($Target)
}

function Backup-XmppCredential([string]$Target, [string]$Directory) {
    Initialize-XmppCredentialApi
    $secret = [AnyKeepAdmin.WindowsCredentials]::Read($Target)
    if ($null -eq $secret) { return }
    try {
        Add-Type -AssemblyName System.Security
        $protected = [Security.Cryptography.ProtectedData]::Protect($secret, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
        [IO.File]::WriteAllBytes((Join-Path $Directory 'note-key.dpapi'), $protected)
    } finally { [Array]::Clear($secret, 0, $secret.Length) }
    'note-key.dpapi'
}

function Remove-XmppCredential([string]$Target) {
    Initialize-XmppCredentialApi
    [AnyKeepAdmin.WindowsCredentials]::Delete($Target)
}

function Assert-AnyKeepStopped {
    if (Get-Process -Name anykeep, anykeepexe, anykeep_mobile -ErrorAction SilentlyContinue) {
        throw 'Close AnyKeep completely, including its tray application, before applying the reset.'
    }
}

function Invoke-AnyKeepXmppReset {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([switch]$Apply, [string]$DataRoot, [string]$BackupRoot)
    $ErrorActionPreference = 'Stop'
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'This script supports Windows only.' }
    if (-not $DataRoot) { $DataRoot = Join-Path $env:LOCALAPPDATA 'R-Soft\AnyKeep' }
    if (-not $BackupRoot) { $BackupRoot = Join-Path $PSScriptRoot '..\build\_diagnostics' }
    $settings = Get-XmppResetSettings
    $plan = Get-XmppResetPlan $DataRoot $settings.Jid $settings.InstanceId
    $keyExists = Test-XmppCredential $plan.CredentialTarget
    Write-Host "Account hash: $($plan.AccountHash); note encryption key present: $keyExists"
    $plan.Files | Select-Object RelativePath, Length | Format-Table -AutoSize | Out-Host
    if (-not $Apply) { Write-Host 'Preview only. Use -Apply to back up and reset this local state.'; return }
    if (-not $PSCmdlet.ShouldProcess("AnyKeep XMPP account $($plan.AccountHash)", 'Back up and remove note key, cache and OMEMO state')) { return }
    Assert-AnyKeepStopped
    if ($plan.Files.Count -eq 0 -and -not $keyExists) {
        Write-Host 'The note key, XMPP cache and OMEMO state are already absent.'
        return [PSCustomObject]@{ FilesMoved = 0; KeyRemoved = $false; BackupDirectory = $null }
    }
    $backupParent = [IO.Path]::GetFullPath($BackupRoot).TrimEnd('\', '/')
    $backup = [IO.Path]::GetFullPath((Join-Path $backupParent ('xmpp-reset-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))))
    Assert-XmppResetTarget $backupParent $backup
    if ($backup.StartsWith($plan.DataRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Choose a backup directory outside the application data directory.'
    }
    $null = New-Item -ItemType Directory -Path $backup -Force
    $credentialBackup = Backup-XmppCredential $plan.CredentialTarget $backup
    [PSCustomObject]@{
        SchemaVersion = 1
        DataRoot = $plan.DataRoot
        CredentialTarget = $plan.CredentialTarget
        CredentialBackup = $credentialBackup
        CredentialProtection = 'Windows DPAPI CurrentUser'
        Files = @($plan.Files | Select-Object RelativePath, Length)
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'restore-manifest.json') -Encoding UTF8
    $moved = @()
    try {
        foreach ($file in $plan.Files) {
            # Revalidate immediately before moving; a plan is not a path-safety exemption.
            Assert-XmppResetTarget $plan.DataRoot $file.Source
            $destination = [IO.Path]::GetFullPath((Join-Path $backup $file.RelativePath))
            Assert-XmppResetTarget $backup $destination
            $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force
            Move-Item -LiteralPath $file.Source -Destination $destination
            $moved += [PSCustomObject]@{ Source = $file.Source; Backup = $destination }
        }
        Assert-AnyKeepStopped
        # Delete last: a keychain failure rolls back the preceding file moves.
        $keyRemoved = Remove-XmppCredential $plan.CredentialTarget
    } catch {
        foreach ($file in $moved) {
            Assert-XmppResetTarget $plan.DataRoot $file.Source
            Assert-XmppResetTarget $backup $file.Backup
            if ((Test-Path -LiteralPath $file.Backup) -and -not (Test-Path -LiteralPath $file.Source)) {
                Move-Item -LiteralPath $file.Backup -Destination $file.Source
            }
        }
        throw
    }
    [PSCustomObject]@{ FilesMoved = $moved.Count; KeyRemoved = $keyRemoved; BackupDirectory = $backup }
}

# Dot-sourcing loads helpers for isolated fixture tests without accessing settings or credentials.
if ($MyInvocation.InvocationName -ne '.') { Invoke-AnyKeepXmppReset @PSBoundParameters }
