#requires -Version 5.1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\admin\reset-local-xmpp.ps1')

function Assert-ResetTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-ResetThrows([scriptblock]$Action, [string]$Pattern) {
    $caught = $false
    try { $null = & $Action } catch {
        $caught = $true
        if ($_.Exception.Message -notmatch $Pattern) { throw }
    }
    Assert-ResetTest $caught "Expected failure matching: $Pattern"
}

# Replace all OS/account/keychain dependencies before invoking any reset helpers.
function Get-XmppResetSettings {
    [PSCustomObject]@{ Jid = ' tester@example.invalid/desktop '; InstanceId = $script:fixtureInstance }
}
function Test-XmppCredential([string]$Target) {
    Assert-ResetTest ($Target -ceq 'xmpp-storage-master-key-v1:tester@example.invalid') 'Unexpected credential target'
    $script:fixtureKeyExists
}
function Backup-XmppCredential([string]$Target, [string]$Directory) {
    $script:fixtureBackupCalls++
    if (Test-XmppCredential $Target) {
        [IO.File]::WriteAllText((Join-Path $Directory 'note-key.dpapi'), 'protected-fixture')
        'note-key.dpapi'
    }
}
function Remove-XmppCredential([string]$Target) {
    $script:fixtureDeleteCalls++
    $existed = Test-XmppCredential $Target
    if ($script:fixtureDeleteFails) { throw 'Fixture keychain failure' }
    $script:fixtureKeyExists = $false
    $existed
}
function Assert-AnyKeepStopped {
    if ($script:fixtureRunning) { throw 'Close AnyKeep before resetting' }
}

$fixtureParent = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\_diagnostics'))
$fixtureBase = Join-Path $fixtureParent ('xmpp-reset-tests-' + [Guid]::NewGuid().ToString('N'))
$script:fixtureInstance = '11111111-2222-3333-4444-555555555555'
$script:fixtureLink = $null
$caseNumber = 0
$passed = 0

function New-ResetFixture {
    $script:caseNumber++
    $script:fixtureData = Join-Path $fixtureBase ("case-$caseNumber\data")
    $script:fixtureBackups = Join-Path $fixtureBase ("case-$caseNumber\backups")
    $script:fixtureKeyExists = $true
    $script:fixtureDeleteFails = $false
    $script:fixtureRunning = $false
    $script:fixtureBackupCalls = 0
    $script:fixtureDeleteCalls = 0
    # Known SHA256 prefix of tester@example.invalid; include state for both backends and two PubSub nodes.
    $script:fixtureResetFiles = @(
        'xmpp-omemo-dced4e0a5a37d757.state',
        'xmpp-omemo-dced4e0a5a37d757.state.trust',
        'xmpp-omemo-dced4e0a5a37d757.state.iris',
        'xmpp-omemo-dced4e0a5a37d757.state.iris.trust',
        "remote-cache\xmpppubsub\$fixtureInstance-0000000000000000.cache",
        "remote-cache\xmpppubsub\$fixtureInstance-1111111111111111.cache"
    )
    $script:fixtureKeepFiles = @(
        'xmpp-omemo-aaaaaaaaaaaaaaaa.state.iris',
        'xmpp-omemo-dced4e0a5a37d757.state.iris.jinglepub',
        'remote-cache\xmpppubsub\aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-0000000000000000.cache',
        'drafts\draft.bin',
        'media\blob.bin'
    )
    foreach ($relative in ($fixtureResetFiles + $fixtureKeepFiles)) {
        $path = Join-Path $fixtureData $relative
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force
        [IO.File]::WriteAllText($path, 'fixture:' + $relative)
    }
}

function Assert-FixtureIntact {
    foreach ($relative in ($fixtureResetFiles + $fixtureKeepFiles)) {
        Assert-ResetTest (([IO.File]::ReadAllText((Join-Path $fixtureData $relative))) -ceq ('fixture:' + $relative)) "Changed file: $relative"
    }
    Assert-ResetTest $fixtureKeyExists 'Key was removed unexpectedly'
}

try {
    New-ResetFixture
    $null = Invoke-AnyKeepXmppReset -DataRoot $fixtureData -BackupRoot $fixtureBackups
    Assert-FixtureIntact
    Assert-ResetTest (-not (Test-Path -LiteralPath $fixtureBackups)) 'Preview created a backup'
    Assert-ResetTest ($fixtureBackupCalls -eq 0 -and $fixtureDeleteCalls -eq 0) 'Preview touched credentials'
    $passed++

    $null = Invoke-AnyKeepXmppReset -Apply -WhatIf -DataRoot $fixtureData -BackupRoot $fixtureBackups
    Assert-FixtureIntact
    Assert-ResetTest (-not (Test-Path -LiteralPath $fixtureBackups)) 'WhatIf created a backup'
    Assert-ResetTest ($fixtureBackupCalls -eq 0 -and $fixtureDeleteCalls -eq 0) 'WhatIf touched credentials'
    $passed++

    $result = Invoke-AnyKeepXmppReset -Apply -DataRoot $fixtureData -BackupRoot $fixtureBackups
    Assert-ResetTest ($result.FilesMoved -eq 6 -and $result.KeyRemoved) 'Reset did not remove the intended state'
    foreach ($relative in $fixtureResetFiles) {
        Assert-ResetTest (-not (Test-Path -LiteralPath (Join-Path $fixtureData $relative))) "Active state survived: $relative"
        Assert-ResetTest ([IO.File]::ReadAllText((Join-Path $result.BackupDirectory $relative)) -ceq ('fixture:' + $relative)) "Backup mismatch: $relative"
    }
    foreach ($relative in $fixtureKeepFiles) {
        Assert-ResetTest ([IO.File]::ReadAllText((Join-Path $fixtureData $relative)) -ceq ('fixture:' + $relative)) "Unrelated data changed: $relative"
    }
    $manifest = Get-Content -LiteralPath (Join-Path $result.BackupDirectory 'restore-manifest.json') -Raw | ConvertFrom-Json
    Assert-ResetTest ($manifest.CredentialBackup -eq 'note-key.dpapi' -and $manifest.Files.Count -eq 6) 'Invalid backup manifest'
    Assert-ResetTest (Test-Path -LiteralPath (Join-Path $result.BackupDirectory 'note-key.dpapi')) 'Missing protected key backup'
    $passed++

    $result = Invoke-AnyKeepXmppReset -Apply -DataRoot $fixtureData -BackupRoot $fixtureBackups
    Assert-ResetTest ($result.FilesMoved -eq 0 -and -not $result.KeyRemoved -and -not $result.BackupDirectory) 'Repeated reset was not a no-op'
    $passed++

    New-ResetFixture
    $script:fixtureDeleteFails = $true
    Assert-ResetThrows { Invoke-AnyKeepXmppReset -Apply -DataRoot $fixtureData -BackupRoot $fixtureBackups } 'Fixture keychain failure'
    Assert-FixtureIntact
    $passed++

    New-ResetFixture
    $script:fixtureRunning = $true
    Assert-ResetThrows { Invoke-AnyKeepXmppReset -Apply -DataRoot $fixtureData -BackupRoot $fixtureBackups } 'Close AnyKeep'
    Assert-FixtureIntact
    Assert-ResetTest (-not (Test-Path -LiteralPath $fixtureBackups)) 'Running-app refusal created a backup'
    $passed++

    New-ResetFixture
    Assert-ResetThrows { Invoke-AnyKeepXmppReset -Apply -DataRoot $fixtureData -BackupRoot $fixtureData } 'outside the application data'
    Assert-FixtureIntact
    Assert-ResetTest ($fixtureBackupCalls -eq 0 -and $fixtureDeleteCalls -eq 0) 'Unsafe backup path touched credentials'
    $passed++

    Assert-ResetThrows { Get-XmppResetPlan $fixtureData 'tester@example.invalid' '..\escape' } 'Invalid XMPP storage instance'
    Assert-ResetThrows { Assert-XmppResetTarget $fixtureData ([IO.Path]::GetFullPath((Join-Path $fixtureData '..\outside'))) } 'outside the selected data'
    $passed++

    # An ancestor junction must be refused, even when the state file itself is regular.
    $script:fixtureLink = Join-Path $fixtureData 'linked-data'
    $null = New-Item -ItemType Junction -Path $fixtureLink -Target (Join-Path $fixtureData 'remote-cache')
    Assert-ResetThrows { Assert-XmppResetTarget $fixtureData (Join-Path $fixtureLink "xmpppubsub\$fixtureInstance-0000000000000000.cache") } 'linked reset path'
    $passed++
    Write-Output "PASS: $passed isolated XMPP reset checks; no real settings or credentials accessed."
} finally {
    if ($script:fixtureLink -and (Test-Path -LiteralPath $script:fixtureLink)) {
        Assert-ResetTest ($script:fixtureLink.StartsWith($fixtureBase + '\', [StringComparison]::OrdinalIgnoreCase)) 'Unexpected junction cleanup path'
        [IO.Directory]::Delete($script:fixtureLink)
    }
    if (Test-Path -LiteralPath $fixtureBase) {
        $resolvedFixture = (Resolve-Path -LiteralPath $fixtureBase).Path
        Assert-ResetTest ($resolvedFixture.StartsWith($fixtureParent + '\', [StringComparison]::OrdinalIgnoreCase)) 'Unexpected fixture cleanup path'
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
