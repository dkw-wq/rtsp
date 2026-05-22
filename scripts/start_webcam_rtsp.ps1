param(
    [string]$CameraName = "HP True Vision FHD Camera",
    [string]$SecondCameraName = "Logi C270 HD WebCam",
    [string]$AudioName = "麦克风阵列 (2- 适用于数字麦克风的英特尔® 智音技术)",
    [string]$RtspUrl = "rtsp://127.0.0.1:8554/webcam",
    [string]$SecondRtspUrl = "rtsp://127.0.0.1:8554/webcam2",
    [string]$AudioRtspUrl = "rtsp://127.0.0.1:8554/audio",
    [string]$VideoSize = "1280x720",
    [string]$SecondVideoSize = "",
    [int]$Framerate = 30,
    [switch]$Dual,
    [switch]$NoAudio,
    [switch]$FfmpegDebug
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$mediaMtxDir = Join-Path $workspaceRoot "mediamtx"
$mediaMtxExe = Join-Path $mediaMtxDir "mediamtx.exe"
$logDir = Join-Path $mediaMtxDir "logs"
$mediaMtxPidFile = Join-Path $logDir "mediamtx.pid"
$ffmpegPidFiles = @(
    (Join-Path $logDir "ffmpeg-webcam.pid")
)
if ($Dual) {
    $ffmpegPidFiles += (Join-Path $logDir "ffmpeg-webcam2.pid")
    if (!$NoAudio -and ![string]::IsNullOrWhiteSpace($AudioName)) {
        $ffmpegPidFiles += (Join-Path $logDir "ffmpeg-audio.pid")
    }
}

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if (!(Test-Path $mediaMtxExe)) {
    throw "MediaMTX not found: $mediaMtxExe"
}

$mediaMtxProcess = Get-Process mediamtx -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $mediaMtxExe } |
    Select-Object -First 1

if (!$mediaMtxProcess) {
    $mediaMtxProcess = Start-Process `
        -FilePath $mediaMtxExe `
        -WorkingDirectory $mediaMtxDir `
        -RedirectStandardOutput (Join-Path $logDir "mediamtx.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "mediamtx.stderr.log") `
        -WindowStyle Hidden `
        -PassThru
    Set-Content -Path $mediaMtxPidFile -Value $mediaMtxProcess.Id
    Start-Sleep -Seconds 2
}

foreach ($ffmpegPidFile in $ffmpegPidFiles) {
    if (Test-Path $ffmpegPidFile) {
        $oldPid = Get-Content $ffmpegPidFile -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($oldPid -and (Get-Process -Id $oldPid -ErrorAction SilentlyContinue)) {
            Write-Host "Publisher watchdog is already running from PID file: $ffmpegPidFile"
            Write-Host "Run scripts\stop_webcam_rtsp.ps1 before restarting publishers."
            exit 0
        }
        Remove-Item $ffmpegPidFile -Force
    }
}

function Start-WebcamPublisher {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Size,
        [Parameter(Mandatory = $true)][string]$PidFile,
        [Parameter(Mandatory = $true)][string]$LogPrefix,
        [string]$AudioDeviceName = "",
        [switch]$DisableAudio,
        [switch]$DebugLog
    )

    $watcherScript = Join-Path $PSScriptRoot "watch_webcam_publisher.ps1"
    $watcherArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$watcherScript`"",
        "-Mode", "video",
        "-Name", "`"$Name`"",
        "-Url", "`"$Url`"",
        "-LogDir", "`"$logDir`"",
        "-LogPrefix", "`"$LogPrefix`"",
        "-Size", "`"$Size`"",
        "-Framerate", "$Framerate"
    )
    if (![string]::IsNullOrWhiteSpace($AudioDeviceName)) {
        $watcherArgs += @("-AudioDeviceName", "`"$AudioDeviceName`"")
    }
    if ($DisableAudio) {
        $watcherArgs += "-DisableAudio"
    }
    if ($DebugLog) {
        $watcherArgs += "-DebugLog"
    }

    $watcherProcess = Start-Process `
        -FilePath "powershell" `
        -ArgumentList ($watcherArgs -join " ") `
        -WorkingDirectory $mediaMtxDir `
        -WindowStyle Hidden `
        -PassThru

    Set-Content -Path $PidFile -Value $watcherProcess.Id
    return $watcherProcess
}

function Start-AudioPublisher {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$PidFile,
        [Parameter(Mandatory = $true)][string]$LogPrefix,
        [switch]$DebugLog
    )

    $watcherScript = Join-Path $PSScriptRoot "watch_webcam_publisher.ps1"
    $watcherArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$watcherScript`"",
        "-Mode", "audio",
        "-Name", "`"$Name`"",
        "-Url", "`"$Url`"",
        "-LogDir", "`"$logDir`"",
        "-LogPrefix", "`"$LogPrefix`""
    )
    if ($DebugLog) {
        $watcherArgs += "-DebugLog"
    }

    $watcherProcess = Start-Process `
        -FilePath "powershell" `
        -ArgumentList ($watcherArgs -join " ") `
        -WorkingDirectory $mediaMtxDir `
        -WindowStyle Hidden `
        -PassThru

    Set-Content -Path $PidFile -Value $watcherProcess.Id
    return $watcherProcess
}

$publishers = @()
$publishers += @{
    Process = Start-WebcamPublisher `
        -Name $CameraName `
        -Url $RtspUrl `
        -Size $VideoSize `
        -PidFile $ffmpegPidFiles[0] `
        -LogPrefix "ffmpeg-webcam" `
        -AudioDeviceName $AudioName `
        -DisableAudio:($NoAudio -or $Dual) `
        -DebugLog:$FfmpegDebug
    Url = $RtspUrl
    Log = Join-Path $logDir "ffmpeg-webcam.stderr.log"
}

if ($Dual) {
    $secondSize = if ([string]::IsNullOrWhiteSpace($SecondVideoSize)) {
        $VideoSize
    } else {
        $SecondVideoSize
    }

    $publishers += @{
        Process = Start-WebcamPublisher `
            -Name $SecondCameraName `
            -Url $SecondRtspUrl `
            -Size $secondSize `
            -PidFile $ffmpegPidFiles[1] `
            -LogPrefix "ffmpeg-webcam2" `
            -DisableAudio `
            -DebugLog:$FfmpegDebug
        Url = $SecondRtspUrl
        Log = Join-Path $logDir "ffmpeg-webcam2.stderr.log"
    }

    if (!$NoAudio -and ![string]::IsNullOrWhiteSpace($AudioName)) {
        $publishers += @{
            Process = Start-AudioPublisher `
                -Name $AudioName `
                -Url $AudioRtspUrl `
                -PidFile (Join-Path $logDir "ffmpeg-audio.pid") `
                -LogPrefix "ffmpeg-audio" `
                -DebugLog:$FfmpegDebug
            Url = $AudioRtspUrl
            Log = Join-Path $logDir "ffmpeg-audio.stderr.log"
        }
    }
}

Start-Sleep -Seconds 8

foreach ($publisher in $publishers) {
    if ($publisher.Process.HasExited) {
        Get-Content $publisher.Log -ErrorAction SilentlyContinue
        foreach ($startedPublisher in $publishers) {
            if (!$startedPublisher.Process.HasExited) {
                Stop-Process -Id $startedPublisher.Process.Id -Force
            }
        }
        foreach ($pidFile in $ffmpegPidFiles) {
            if (Test-Path $pidFile) {
                Remove-Item $pidFile -Force
            }
        }
        throw "Publisher watchdog failed to start: $($publisher.Url)"
    }
}

Write-Host "MediaMTX PID: $($mediaMtxProcess.Id)"
foreach ($publisher in $publishers) {
    Write-Host "Publisher watchdog PID: $($publisher.Process.Id)"
    Write-Host "RTSP URL: $($publisher.Url)"
}

if ($Dual) {
    Write-Host "Player config rtsp_urls:"
    foreach ($publisher in $publishers) {
        Write-Host "  - `"$($publisher.Url)`""
    }
}
