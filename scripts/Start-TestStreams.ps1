<#
.SYNOPSIS
    Starts MediaMTX and lightweight RTSP test streams for the SightFlow VMS demo.

.DESCRIPTION
    Starts MediaMTX (if not already running, tracked by this script) and
    publishes a cheap synthetic "testsrc" video (320x240, 10fps -- not
    mandelbrot or anything CPU-heavy) to one or both fixed demo channels
    ("test", "test2") via FFmpeg.

    All paths are resolved relative to this script's own location
    ($PSScriptRoot), never a machine-specific absolute path. MediaMTX and
    FFmpeg are expected at:
        <repo root>\MediaMTX\mediamtx.exe (+ mediamtx.yml)
        <repo root>\ffmpeg\bin\ffmpeg.exe
    Neither is included in the repository (see README.md) -- place your own
    copies there first.

    Every process this script starts is recorded (PID + expected process
    name) in .demo-state.json next to this script, so Stop-TestStreams.ps1
    can later stop exactly those processes and no others -- never an
    unrelated mediamtx.exe/ffmpeg.exe instance the user happens to have
    running for something else.

.PARAMETER Channel
    Which channel(s) to (re)start the test publisher for. Defaults to both.
    MediaMTX itself is always ensured to be running regardless of this
    parameter (a channel publisher needs it); if already tracked and alive,
    it is left untouched.

.EXAMPLE
    .\Start-TestStreams.ps1
    Starts MediaMTX plus both "test" and "test2" publishers.

.EXAMPLE
    .\Start-TestStreams.ps1 -Channel test2
    Starts only the "test2" publisher (after Stop-TestStreams.ps1 -Channel test2),
    leaving MediaMTX and "test" untouched if already running.
#>
param(
    [ValidateSet('test', 'test2')]
    [string[]]$Channel = @('test', 'test2')
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$mediamtxExe = Join-Path $repoRoot 'MediaMTX\mediamtx.exe'
$mediamtxYml = Join-Path $repoRoot 'MediaMTX\mediamtx.yml'
$ffmpegExe = Join-Path $repoRoot 'ffmpeg\bin\ffmpeg.exe'
$logsDir = Join-Path $PSScriptRoot 'logs'
$stateFile = Join-Path $PSScriptRoot '.demo-state.json'

if (-not (Test-Path $logsDir)) {
    New-Item -ItemType Directory -Path $logsDir -Force | Out-Null
}

function Get-DemoState {
    if (Test-Path $stateFile) {
        $raw = Get-Content $stateFile -Raw | ConvertFrom-Json
        $state = @{}
        foreach ($prop in $raw.PSObject.Properties) {
            $state[$prop.Name] = @{ pid = $prop.Value.pid; processName = $prop.Value.processName; log = $prop.Value.log }
        }
        return $state
    }
    return @{}
}

function Save-DemoState($state) {
    $state | ConvertTo-Json -Depth 4 | Set-Content -Path $stateFile -Encoding utf8
}

function Test-TrackedProcessAlive($entry) {
    if (-not $entry) { return $false }
    $proc = Get-Process -Id $entry.pid -ErrorAction SilentlyContinue
    return ($proc -and $proc.ProcessName -eq $entry.processName)
}

$state = Get-DemoState

# --- MediaMTX ---
if (Test-TrackedProcessAlive $state['mediamtx']) {
    Write-Output "MediaMTX already running (PID $($state['mediamtx'].pid)) -- leaving it alone."
} else {
    if (-not (Test-Path $mediamtxExe)) {
        throw "MediaMTX not found at '$mediamtxExe'. Download it (see README.md) and place mediamtx.exe (+ mediamtx.yml) there first."
    }
    $mediamtxLog = Join-Path $logsDir 'mediamtx.log'
    $proc = Start-Process -FilePath $mediamtxExe -ArgumentList @($mediamtxYml) `
        -WorkingDirectory (Split-Path -Parent $mediamtxExe) `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $mediamtxLog -RedirectStandardError "$mediamtxLog.err"
    Start-Sleep -Seconds 2
    $state['mediamtx'] = @{ pid = $proc.Id; processName = 'mediamtx'; log = $mediamtxLog }
    Write-Output "Started MediaMTX (PID $($proc.Id)), log: $mediamtxLog"
}

# --- Test publishers ---
if (-not (Test-Path $ffmpegExe)) {
    throw "FFmpeg not found at '$ffmpegExe'. Download it (see README.md) and place ffmpeg.exe under ffmpeg\bin\ first."
}

foreach ($ch in $Channel) {
    if (Test-TrackedProcessAlive $state[$ch]) {
        Write-Output "'$ch' publisher already running (PID $($state[$ch].pid)) -- run Stop-TestStreams.ps1 -Channel $ch first to restart it."
        continue
    }

    $chLog = Join-Path $logsDir "$ch.log"
    $ffArgs = @(
        '-re', '-f', 'lavfi', '-i', 'testsrc=size=320x240:rate=10',
        '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '20',
        '-f', 'rtsp', "rtsp://127.0.0.1:8554/$ch"
    )
    $proc = Start-Process -FilePath $ffmpegExe -ArgumentList $ffArgs `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $chLog -RedirectStandardError "$chLog.err"
    $state[$ch] = @{ pid = $proc.Id; processName = 'ffmpeg'; log = $chLog }
    Write-Output "Started '$ch' test publisher (PID $($proc.Id)), log: $chLog"
}

Save-DemoState $state

Write-Output ''
Write-Output 'Verify with: curl.exe http://127.0.0.1:9997/v3/paths/list'
