<#
.SYNOPSIS
    Stops exactly the MediaMTX/test-publisher processes Start-TestStreams.ps1 started.

.DESCRIPTION
    Reads .demo-state.json (written by Start-TestStreams.ps1, next to this
    script) and stops only the processes recorded there -- and only after
    confirming the recorded PID still belongs to a process with the expected
    name (mediamtx / ffmpeg). This is a safety check against PID reuse: if
    the original process already exited and Windows handed that PID to some
    unrelated process in the meantime, this script will not touch it.

    Never stops a mediamtx.exe/ffmpeg.exe instance this script (or its
    Start- counterpart) did not itself start.

.PARAMETER Channel
    Which test-channel publisher(s) to stop ("test" and/or "test2"). When
    omitted, stops everything currently tracked, including MediaMTX itself.
    When given, MediaMTX is left running (so you can restart just one
    channel's publisher without tearing down MediaMTX too).

.EXAMPLE
    .\Stop-TestStreams.ps1 -Channel test2
    Stops only the "test2" publisher.

.EXAMPLE
    .\Stop-TestStreams.ps1
    Stops everything this script's Start- counterpart is currently tracking
    (both publishers and MediaMTX).
#>
param(
    [ValidateSet('test', 'test2')]
    [string[]]$Channel
)

$ErrorActionPreference = 'Stop'

$stateFile = Join-Path $PSScriptRoot '.demo-state.json'

if (-not (Test-Path $stateFile)) {
    Write-Output 'Nothing tracked (.demo-state.json not found) -- nothing to stop.'
    return
}

$raw = Get-Content $stateFile -Raw | ConvertFrom-Json
$state = @{}
foreach ($prop in $raw.PSObject.Properties) {
    $state[$prop.Name] = @{ pid = $prop.Value.pid; processName = $prop.Value.processName; log = $prop.Value.log }
}

if ($Channel) {
    $targets = $Channel
} else {
    # @(...) copies the keys out into a plain array -- $state.Keys itself is
    # a live view, and the loop below calls $state.Remove($key), which would
    # otherwise mutate the very collection being enumerated.
    $targets = @($state.Keys)
}

foreach ($key in $targets) {
    $entry = $state[$key]
    if (-not $entry) {
        Write-Output "'$key' is not currently tracked -- nothing to stop."
        continue
    }

    $proc = Get-Process -Id $entry.pid -ErrorAction SilentlyContinue
    if ($proc -and $proc.ProcessName -eq $entry.processName) {
        Stop-Process -Id $entry.pid -Force
        Write-Output "Stopped '$key' (PID $($entry.pid), $($entry.processName))."
    } else {
        Write-Output "'$key' (PID $($entry.pid)) is no longer running (or the PID was reused by something else) -- skipping, not touching it."
    }

    $state.Remove($key)
}

if ($state.Count -eq 0) {
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
} else {
    $state | ConvertTo-Json -Depth 4 | Set-Content -Path $stateFile -Encoding utf8
}
