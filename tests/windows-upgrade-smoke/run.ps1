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

Write-Host "Disposable installation root: $root"
Write-Host "Downloading the real current stable GitHub ZIP through WindowsUpdateController..."
$probe = Start-Process -FilePath (Join-Path $bin 'matterleast.exe') -NoNewWindow -Wait -PassThru
if ($probe.ExitCode -ne 0) {
    throw "WindowsUpdateController failed to download/prepare the published update (exit $($probe.ExitCode))"
}

$preparedPath = Join-Path $root 'staging/prepared.json'
if (-not (Test-Path $preparedPath)) { throw 'Controller did not persist staging/prepared.json' }
$prepared = Get-Content $preparedPath -Raw | ConvertFrom-Json
$id = [string]$prepared.identity
if ($id -notmatch '^v[0-9]+\.[0-9]+(\.[0-9]+)?$') { throw "Invalid prepared stable identity: $id" }
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

Write-Host "Checking rollback after deliberately failed new-version startup..."
Set-Content -Path (Join-Path $root 'current.version') -Value $oldId -NoNewline -Encoding utf8
Copy-Item (Join-Path $build 'update-bad-stub.exe') (Join-Path $targetBin 'matterleast.exe') -Force
$code = Invoke-VersionSwitch $id
if ($code -eq 0) { throw 'Failed startup was incorrectly reported as an upgrade success' }
if ((Current-Version) -ne $oldId) { throw 'Rollback failed to restore the old version' }

Write-Host "PASS: live download, SHA-256/ZIP preparation, native switch, health probe and rollback."
Remove-Item $root -Recurse -Force
