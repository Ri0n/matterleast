param(
    [Parameter(Mandatory = $true)][string]$BuildDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$build = (Resolve-Path $BuildDir).Path
$root = Join-Path $env:RUNNER_TEMP "matterleast-update-smoke-$PID"
$oldId = 'v1.0'
$bin = Join-Path $root "versions/$oldId/MatterLeast/bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null
Copy-Item (Join-Path $build 'update-download-probe.exe') (Join-Path $bin 'matterleast.exe')
Copy-Item (Join-Path $build 'MatterLeastUpdater.exe') (Join-Path $bin 'MatterLeastUpdater.exe')
Copy-Item (Join-Path $build 'MatterLeastLauncher.exe') (Join-Path $root 'MatterLeastLauncher.exe')
Set-Content -Path (Join-Path $root 'current.version') -Value $oldId -NoNewline -Encoding utf8

# Reproduce the user case: a previous download passed SHA-256 but extraction
# failed, leaving staging/<buildId>.zip. A second successful transfer must be
# able to replace that file instead of reporting an unrelated hash/I/O error.
$manifest = Invoke-RestMethod -Uri 'https://github.com/Ri0n/matterleast/releases/download/updates-nightly/windows-x64.json'
$pendingId = [string]$manifest.buildId
if ($pendingId -notmatch '^nightly-[0-9]{8}-[0-9]{6}-[a-f0-9]{7,40}$') {
    throw 'Live manifest did not provide a valid nightly buildId'
}
$priorZip = Join-Path $root "staging/$pendingId.zip"
New-Item -ItemType Directory -Force -Path (Split-Path $priorZip) | Out-Null
[System.IO.File]::WriteAllBytes($priorZip, [System.Text.Encoding]::ASCII.GetBytes(
    'leftover ZIP from a previous failed extraction'))
Write-Host "Simulating previously downloaded ZIP at: $priorZip"
Write-Host "Disposable installation root: $root"
Write-Host "Downloading the current nightly GitHub ZIP through WindowsUpdateController..."
# Invoke a console-subsystem smoke binary directly so Qt's qCritical
# output (HTTP response, TLS error, etc.) survives in Actions logs.
& (Join-Path $bin 'matterleast.exe') 2>&1 | Out-Host
$probeExit = $LASTEXITCODE
$diagnostics = Join-Path $bin 'smoke.log'
if (Test-Path $diagnostics) {
    Write-Host '----- Native updater diagnostic log -----'
    Get-Content $diagnostics
    Write-Host '----- End native updater diagnostic log -----'
} else {
    Write-Warning 'No native updater diagnostics produced'
}
if ($probeExit -ne 0) {
    throw "WindowsUpdateController failed to download/prepare the published update (exit $probeExit)"
}

$preparedPath = Join-Path $root 'staging/prepared.json'
if (-not (Test-Path $preparedPath)) { throw 'Controller did not persist staging/prepared.json' }
$prepared = Get-Content $preparedPath -Raw | ConvertFrom-Json
$id = [string]$prepared.identity
if ($id -notmatch '^nightly-[0-9]{8}-[0-9]{6}-[a-f0-9]{7,40}$') {
    throw "Invalid prepared nightly identity: $id"
}
$targetBin = Join-Path $root "versions/$id/MatterLeast/bin"
foreach ($required in @('matterleast.exe','MatterLeastUpdater.exe','Qt6Core.dll')) {
    if (-not (Test-Path (Join-Path $targetBin $required))) {
        throw "Real downloaded ZIP did not install $required"
    }
}
$download = Join-Path $root "staging/$id.zip"
if (-not (Test-Path $download)) { throw "Downloaded ZIP missing: $download" }
Write-Host "Real GitHub ZIP downloaded, checked by controller (SHA256/length) and installed: $id"

# Replace ONLY the GUI entrypoints with tiny smoke applications. The real
# production launcher and updater are used unchanged from this source tree.
Copy-Item (Join-Path $build 'update-good-stub.exe') (Join-Path $bin 'matterleast.exe') -Force
Copy-Item (Join-Path $build 'update-good-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force
Copy-Item (Join-Path $build 'MatterLeastUpdater.exe') (Join-Path $targetBin 'MatterLeastUpdater.exe') -Force

function Invoke-VersionSwitch([string]$targetId) {
    # Mimic a real exiting GUI process; the updater must wait for it.
    $old = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Seconds 2'
    ) -PassThru
    $switcher = Join-Path $root "versions/$targetId/MatterLeast/bin/MatterLeastUpdater.exe"
    $args = @('--root', ('"' + $root + '"'), '--target', $targetId,
              '--wait-pid', [string]$old.Id)
    $result = Start-Process -FilePath $switcher -ArgumentList $args -PassThru -Wait
    $old.WaitForExit()
    return $result.ExitCode
}

function Current-Version {
    return (Get-Content (Join-Path $root 'current.version') -Raw).Trim()
}

Write-Host "Applying upgrade $oldId -> $id using the native updater..."
$code = Invoke-VersionSwitch $id
if ($code -ne 0) { throw "Native upgrade failed (exit $code)" }
if ((Current-Version) -ne $id) { throw 'current.version did not switch to the downloaded version' }
if ((Get-Content (Join-Path $root 'previous.version') -Raw).Trim() -ne $oldId) {
    throw 'previous.version was not retained for rollback'
}
if (Test-Path $preparedPath) { throw 'Prepared state was not cleaned up after healthy start' }

# Exercise the same real GitHub manifest again *from the installed version*.
# It must report no updates, not download/prepare the identical nightly twice.
Write-Host "Checking that the installed nightly does not download itself..."
Copy-Item (Join-Path $build 'update-download-probe.exe') (Join-Path $targetBin 'matterleast.exe') -Force
& (Join-Path $targetBin 'matterleast.exe') --expect-no-update 2>&1 | Out-Host
$checkExit = $LASTEXITCODE
if (Test-Path (Join-Path $targetBin 'smoke.log')) {
    Get-Content (Join-Path $targetBin 'smoke.log')
}
if ($checkExit -ne 0) {
    throw "Self-update regression: installed nightly re-downloaded itself (exit $checkExit)"
}
Copy-Item (Join-Path $build 'update-good-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force

Write-Host "Checking rollback after deliberately failed new-version startup..."
Set-Content -Path (Join-Path $root 'current.version') -Value $oldId -NoNewline -Encoding utf8
Copy-Item (Join-Path $build 'update-bad-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force
$code = Invoke-VersionSwitch $id
if ($code -eq 0) { throw 'Failed startup was incorrectly reported as an upgrade success' }
if ((Current-Version) -ne $oldId) { throw 'Rollback failed to restore the old version' }

Write-Host "PASS: live download, SHA-256/ZIP preparation, native switch, no self-update, health probe and rollback."
Remove-Item $root -Recurse -Force
) {
    throw "Live manifest did not provide a valid nightly buildId"
}
$priorZip = Join-Path $root "staging/$pendingId.zip"
New-Item -ItemType Directory -Force -Path (Split-Path $priorZip) | Out-Null
[System.IO.File]::WriteAllBytes($priorZip, [System.Text.Encoding]::ASCII.GetBytes(
    'leftover ZIP from a previous failed extraction'))
Write-Host "Simulating a previously downloaded ZIP at: $priorZip"

Write-Host "Disposable installation root: $root"
Write-Host "Downloading the current nightly GitHub ZIP through WindowsUpdateController..."
# Invoke a console-subsystem smoke binary directly so Qt's qCritical
# output (HTTP response, TLS error, etc.) survives in Actions logs.
& (Join-Path $bin 'matterleast.exe') 2>&1 | Out-Host
$probeExit = $LASTEXITCODE
$diagnostics = Join-Path $bin 'smoke.log'
if (Test-Path $diagnostics) {
    Write-Host '----- Native updater diagnostic log -----'
    Get-Content $diagnostics
    Write-Host '----- End native updater diagnostic log -----'
} else {
    Write-Warning 'No native updater diagnostics produced'
}
if ($probeExit -ne 0) {
    throw "WindowsUpdateController failed to download/prepare the published update (exit $probeExit)"
}

$preparedPath = Join-Path $root 'staging/prepared.json'
if (-not (Test-Path $preparedPath)) { throw 'Controller did not persist staging/prepared.json' }
$prepared = Get-Content $preparedPath -Raw | ConvertFrom-Json
$id = [string]$prepared.identity
if ($id -notmatch '^nightly-[0-9]{8}-[0-9]{6}-[a-f0-9]{7,40}$') {
    throw "Invalid prepared nightly identity: $id"
}
$targetBin = Join-Path $root "versions/$id/MatterLeast/bin"
foreach ($required in @('matterleast.exe','MatterLeastUpdater.exe','Qt6Core.dll')) {
    if (-not (Test-Path (Join-Path $targetBin $required))) {
        throw "Real downloaded ZIP did not install $required"
    }
}
$download = Join-Path $root "staging/$id.zip"
if (-not (Test-Path $download)) { throw "Downloaded ZIP missing: $download" }
Write-Host "Real GitHub ZIP downloaded, checked by controller (SHA256/length) and installed: $id"

# Replace ONLY the GUI entrypoints with tiny smoke applications. The real
# production launcher and updater are used unchanged from this source tree.
Copy-Item (Join-Path $build 'update-good-stub.exe') (Join-Path $bin 'matterleast.exe') -Force
Copy-Item (Join-Path $build 'update-good-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force
Copy-Item (Join-Path $build 'MatterLeastUpdater.exe') (Join-Path $targetBin 'MatterLeastUpdater.exe') -Force

function Invoke-VersionSwitch([string]$targetId) {
    # Mimic a real exiting GUI process; the updater must wait for it.
    $old = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Seconds 2'
    ) -PassThru
    $switcher = Join-Path $root "versions/$targetId/MatterLeast/bin/MatterLeastUpdater.exe"
    $args = @('--root', ('"' + $root + '"'), '--target', $targetId,
              '--wait-pid', [string]$old.Id)
    $result = Start-Process -FilePath $switcher -ArgumentList $args -PassThru -Wait
    $old.WaitForExit()
    return $result.ExitCode
}

function Current-Version {
    return (Get-Content (Join-Path $root 'current.version') -Raw).Trim()
}

Write-Host "Applying upgrade $oldId -> $id using the native updater..."
$code = Invoke-VersionSwitch $id
if ($code -ne 0) { throw "Native upgrade failed (exit $code)" }
if ((Current-Version) -ne $id) { throw 'current.version did not switch to the downloaded version' }
if ((Get-Content (Join-Path $root 'previous.version') -Raw).Trim() -ne $oldId) {
    throw 'previous.version was not retained for rollback'
}
if (Test-Path $preparedPath) { throw 'Prepared state was not cleaned up after healthy start' }

# Exercise the same real GitHub manifest again *from the installed version*.
# It must report no updates, not download/prepare the identical nightly twice.
Write-Host "Checking that the installed nightly does not download itself..."
Copy-Item (Join-Path $build 'update-download-probe.exe') (Join-Path $targetBin 'matterleast.exe') -Force
& (Join-Path $targetBin 'matterleast.exe') --expect-no-update 2>&1 | Out-Host
$checkExit = $LASTEXITCODE
if (Test-Path (Join-Path $targetBin 'smoke.log')) {
    Get-Content (Join-Path $targetBin 'smoke.log')
}
if ($checkExit -ne 0) {
    throw "Self-update regression: installed nightly re-downloaded itself (exit $checkExit)"
}
Copy-Item (Join-Path $build 'update-good-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force

Write-Host "Checking rollback after deliberately failed new-version startup..."
Set-Content -Path (Join-Path $root 'current.version') -Value $oldId -NoNewline -Encoding utf8
Copy-Item (Join-Path $build 'update-bad-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force
$code = Invoke-VersionSwitch $id
if ($code -eq 0) { throw 'Failed startup was incorrectly reported as an upgrade success' }
if ((Current-Version) -ne $oldId) { throw 'Rollback failed to restore the old version' }

Write-Host "PASS: live download, SHA-256/ZIP preparation, native switch, no self-update, health probe and rollback."
Remove-Item $root -Recurse -Force
