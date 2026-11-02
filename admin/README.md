# Local XMPP reset on Windows

Close AnyKeep, including its tray application, and run from the repository root:

```powershell
# Preview; this does not modify files or credentials.
.\admin\reset-local-xmpp.ps1

# Back up and reset the configured XMPP account.
.\admin\reset-local-xmpp.ps1 -Apply
```

The script reads the account and storage instance from AnyKeep's Windows
settings. It removes the account's note encryption key from Windows Credential
Manager, moves the Iris/QXmpp OMEMO identity/session/trust files out of the data
directory, and clears all cached PubSub nodes for the configured storage instance.
This lets the next start exercise key recovery and OMEMO initialization again.

Backups go to `build/_diagnostics/xmpp-reset-*`, with a file manifest and a
`note-key.dpapi` credential backup protected for the current Windows user.
The script never prints or writes the plaintext key. File moves are rolled back
if removing the credential fails. `-WhatIf -Apply` previews the operation too.

The connection password, account settings, server notes/devices, drafts, media,
Jingle publication records and other storage instances remain intact. The shared
local-data key and OMEMO file-protection key also remain intact; the latter still
protects Jingle publication records. Previous reset backups are not removed.

`-DataRoot` overrides `%LOCALAPPDATA%\R-Soft\AnyKeep`; `-BackupRoot` changes the
backup parent directory. Linked state files or directories are rejected.

Isolated fixture checks (no real keychain or application data access):

```powershell
powershell -NoProfile -File tests/reset_local_xmpp_test.ps1
```
