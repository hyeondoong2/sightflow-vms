<#
.SYNOPSIS
    Starts MediaMTX and lightweight RTSP test streams for the SightFlow VMS demo.

.DESCRIPTION
    Starts MediaMTX (if not already running, tracked by this script) and
    publishes video to one or both fixed demo channels ("test", "test2") via
    FFmpeg. By default each channel gets a cheap synthetic "testsrc" video
    (320x240, 10fps -- not mandelbrot or anything CPU-heavy); pass -TestFile
    and/or -Test2File to publish a real video file (looped) on that channel
    instead -- see those parameters below.

    All paths are resolved relative to this script's own location
    ($PSScriptRoot), never a machine-specific absolute path. MediaMTX and
    FFmpeg are expected at:
        <repo root>\MediaMTX\mediamtx.exe (+ mediamtx.yml)
        <repo root>\ffmpeg\bin\ffmpeg.exe
    Neither is included in the repository (see README.md) -- place your own
    copies there first. -TestFile/-Test2File are resolved the normal
    PowerShell way (relative to the current directory if not absolute) --
    run this script from the repository root, as the rest of the demo
    procedure (docs/DEMO.md) already assumes.

    Every process this script starts is recorded (PID + expected process
    name) in .demo-state.json next to this script, so Stop-TestStreams.ps1
    can later stop exactly those processes and no others -- never an
    unrelated mediamtx.exe/ffmpeg.exe instance the user happens to have
    running for something else. State is saved after each channel starts
    (not just once at the end), so if one channel fails to start, any
    channel that already started in this same run is still tracked and
    won't be leaked.

.PARAMETER Channel
    Which channel(s) to (re)start the test publisher for. Defaults to both.
    MediaMTX itself is always ensured to be running regardless of this
    parameter (a channel publisher needs it); if already tracked and alive,
    it is left untouched.

.PARAMETER TestFile
    Optional path to a video file to loop and publish on the "test" channel
    instead of the synthetic testsrc pattern. Scaled to 640px wide, 10fps,
    H.264, no audio, with a short keyframe interval (-g 20, docs/DEMO.md --
    without it, a client that (re)connects mid-stream can wait tens of
    seconds for the next keyframe before anything decodes). Ignored if
    "test" is not in -Channel.

.PARAMETER Test2File
    Same as -TestFile, for the "test2" channel.

.EXAMPLE
    .\Start-TestStreams.ps1
    Starts MediaMTX plus both "test" and "test2" publishers, both testsrc.

.EXAMPLE
    .\Start-TestStreams.ps1 -Channel test2
    Starts only the "test2" publisher (after Stop-TestStreams.ps1 -Channel test2),
    leaving MediaMTX and "test" untouched if already running.

.EXAMPLE
    .\Start-TestStreams.ps1 -TestFile .\videos\hallway.mp4 -Test2File .\videos\street.mp4
    Starts MediaMTX plus both publishers, looping these two video files
    instead of testsrc.

.EXAMPLE
    .\Start-TestStreams.ps1 -Channel test2 -Test2File .\videos\street.mp4
    Restarts only "test2", now publishing street.mp4 -- MediaMTX, "test",
    the server, and the client are all left untouched.
#>
param(
    [ValidateSet('test', 'test2')]
    [string[]]$Channel = @('test', 'test2'),

    [string]$TestFile,
    [string]$Test2File
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

    $filePath = switch ($ch) { 'test' { $TestFile } 'test2' { $Test2File } }

    if ($filePath) {
        # Resolved the normal PowerShell way (relative to the current
        # directory if not absolute) -- never hardcoded against this
        # machine. A missing file fails here, before any FFmpeg process is
        # started, and names the channel so it's obvious which -*File was
        # wrong.
        $resolved = Resolve-Path -LiteralPath $filePath -ErrorAction SilentlyContinue
        if (-not $resolved) {
            throw "'$ch' channel: input file not found: '$filePath'"
        }
        # -g 20 (keyframe every 2s at 10fps): without it libx264's default
        # keyframe interval (~250 frames = 25s at 10fps) means a client that
        # (re)connects mid-stream can wait tens of seconds before anything
        # decodes -- measured in docs/DEMO.md. -an: no audio track (this
        # project's RTSP pipeline is video-only end to end).
        $ffArgs = @(
            '-re', '-stream_loop', '-1', '-i', $resolved.Path,
            '-vf', 'scale=640:-2,fps=10', '-an',
            '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '20', '-pix_fmt', 'yuv420p',
            '-f', 'rtsp', "rtsp://127.0.0.1:8554/$ch"
        )
        $sourceDesc = "file '$($resolved.Path)'"
    } else {
        $ffArgs = @(
            '-re', '-f', 'lavfi', '-i', 'testsrc=size=320x240:rate=10',
            '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '20',
            '-f', 'rtsp', "rtsp://127.0.0.1:8554/$ch"
        )
        $sourceDesc = 'testsrc'
    }

    $chLog = Join-Path $logsDir "$ch.log"
    try {
        $proc = Start-Process -FilePath $ffmpegExe -ArgumentList $ffArgs `
            -WindowStyle Hidden -PassThru -ErrorAction Stop `
            -RedirectStandardOutput $chLog -RedirectStandardError "$chLog.err"
    } catch {
        throw "'$ch' channel: failed to start FFmpeg ($sourceDesc): $($_.Exception.Message)"
    }
    $state[$ch] = @{ pid = $proc.Id; processName = 'ffmpeg'; log = $chLog }
    Save-DemoState $state # saved right away -- a later channel's failure in this same run can't leak this one untracked
    Write-Output "Started '$ch' test publisher ($sourceDesc, PID $($proc.Id)), log: $chLog"
}

# Unconditional, not just inside the loop above: covers the case where
# MediaMTX was freshly started this run but every requested channel was
# already running (loop only ever `continue`s, never reaching the
# in-loop save) -- MediaMTX's own state must still end up on disk.
Save-DemoState $state

Write-Output ''
Write-Output 'Verify with: curl.exe http://127.0.0.1:9997/v3/paths/list'
