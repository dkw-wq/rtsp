param(
    [string]$CameraName = "HP True Vision FHD Camera",
    [string]$CameraRtspUrl = "rtsp://127.0.0.1:8554/webcam",
    [string]$FileRtspUrl = "rtsp://127.0.0.1:8554/sample",
    [string]$Mp4Path = "C:\Users\dkw\.a_dkwrtc\rtsp\captures\recording_20260507_155110_304.mp4",
    [string]$VideoSize = "1280x720",
    [int]$Framerate = 30,
    [int]$StreamCount = 40,
    [ValidateSet("on", "off", "config")][string]$FaceDetection = "off",
    [switch]$TranscodeFile,
    [switch]$FfmpegDebug,
    [switch]$StartPlayer
)

$ErrorActionPreference = "Stop"

if ($StreamCount -lt 2) {
    throw "StreamCount must be at least 2 because stream 1 is the webcam and streams 2..N reuse the MP4 RTSP stream."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$mediaMtxDir = Join-Path $workspaceRoot "mediamtx"
$mediaMtxExe = Join-Path $mediaMtxDir "mediamtx.exe"
$logDir = Join-Path $mediaMtxDir "logs"
$mediaMtxPidFile = Join-Path $logDir "mediamtx.pid"
$webcamPidFile = Join-Path $logDir "ffmpeg-webcam.pid"
$filePidFile = Join-Path $logDir "ffmpeg-sample-file.pid"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if (!(Test-Path -LiteralPath $mediaMtxExe)) {
    throw "MediaMTX not found: $mediaMtxExe"
}
if (!(Test-Path -LiteralPath $Mp4Path)) {
    throw "MP4 file not found: $Mp4Path"
}

function Start-MediaMtx {
    $process = Get-Process mediamtx -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $mediaMtxExe } |
        Select-Object -First 1

    if ($process) {
        return $process
    }

    $process = Start-Process `
        -FilePath $mediaMtxExe `
        -WorkingDirectory $mediaMtxDir `
        -RedirectStandardOutput (Join-Path $logDir "mediamtx.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "mediamtx.stderr.log") `
        -WindowStyle Hidden `
        -PassThru
    Set-Content -Path $mediaMtxPidFile -Value $process.Id
    Start-Sleep -Seconds 2
    return $process
}

function Assert-PidFileAvailable {
    param(
        [Parameter(Mandatory = $true)][string]$PidFile,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (!(Test-Path -LiteralPath $PidFile)) {
        return
    }

    $oldPid = Get-Content -LiteralPath $PidFile -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($oldPid -and (Get-Process -Id $oldPid -ErrorAction SilentlyContinue)) {
        Write-Host "$Name is already running from PID file: $PidFile"
        Write-Host "Run scripts\stop_webcam_rtsp.ps1 before restarting."
        exit 0
    }

    Remove-Item -LiteralPath $PidFile -Force
}

function Start-WebcamPublisher {
    $watcherScript = Join-Path $PSScriptRoot "watch_webcam_publisher.ps1"
    $watcherArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$watcherScript`"",
        "-Mode", "video",
        "-Name", "`"$CameraName`"",
        "-Url", "`"$CameraRtspUrl`"",
        "-LogDir", "`"$logDir`"",
        "-LogPrefix", "`"ffmpeg-webcam`"",
        "-Size", "`"$VideoSize`"",
        "-Framerate", "$Framerate",
        "-DisableAudio"
    )
    if ($FfmpegDebug) {
        $watcherArgs += "-DebugLog"
    }

    $process = Start-Process `
        -FilePath "powershell" `
        -ArgumentList ($watcherArgs -join " ") `
        -WorkingDirectory $mediaMtxDir `
        -WindowStyle Hidden `
        -PassThru
    Set-Content -Path $webcamPidFile -Value $process.Id
    return $process
}

function Start-FilePublisher {
    $watcherScript = Join-Path $PSScriptRoot "watch_file_publisher.ps1"
    $watcherArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$watcherScript`"",
        "-Path", "`"$Mp4Path`"",
        "-Url", "`"$FileRtspUrl`"",
        "-LogDir", "`"$logDir`"",
        "-LogPrefix", "`"ffmpeg-sample-file`""
    )
    if ($TranscodeFile) {
        $watcherArgs += "-Transcode"
    }
    if ($FfmpegDebug) {
        $watcherArgs += "-DebugLog"
    }

    $process = Start-Process `
        -FilePath "powershell" `
        -ArgumentList ($watcherArgs -join " ") `
        -WorkingDirectory $mediaMtxDir `
        -WindowStyle Hidden `
        -PassThru
    Set-Content -Path $filePidFile -Value $process.Id
    return $process
}

function New-PlayerUrls {
    $urls = @($CameraRtspUrl)
    for ($index = 2; $index -le $StreamCount; ++$index) {
        $urls += $FileRtspUrl
    }
    return $urls
}

$mediaMtxProcess = Start-MediaMtx
Assert-PidFileAvailable -PidFile $webcamPidFile -Name "Webcam publisher"
Assert-PidFileAvailable -PidFile $filePidFile -Name "MP4 file publisher"

$webcamPublisher = Start-WebcamPublisher
$filePublisher = Start-FilePublisher

Start-Sleep -Seconds 3

$publishers = @(
    @{ Process = $webcamPublisher; Url = $CameraRtspUrl; Log = Join-Path $logDir "ffmpeg-webcam.stderr.log" },
    @{ Process = $filePublisher; Url = $FileRtspUrl; Log = Join-Path $logDir "ffmpeg-sample-file.stderr.log" }
)

foreach ($publisher in $publishers) {
    if ($publisher.Process.HasExited) {
        Get-Content $publisher.Log -ErrorAction SilentlyContinue | Select-Object -Last 40
        foreach ($startedPublisher in $publishers) {
            if (!$startedPublisher.Process.HasExited) {
                Stop-Process -Id $startedPublisher.Process.Id -Force
            }
        }
        Remove-Item -LiteralPath $webcamPidFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $filePidFile -Force -ErrorAction SilentlyContinue
        throw "Publisher watchdog failed to start: $($publisher.Url)"
    }
}

$playerUrls = New-PlayerUrls
$playerExe = Join-Path $repoRoot "build-vcpkg\bin\Release\rtsp_player.exe"
$playerArgs = @()
if ($FaceDetection -ne "config") {
    $playerArgs += @("--face-detection", $FaceDetection)
}
$playerArgs += $playerUrls
$quotedPlayerArgs = ($playerArgs | ForEach-Object { "`"$_`"" }) -join " "

Write-Host "MediaMTX PID: $($mediaMtxProcess.Id)"
Write-Host "Webcam publisher watchdog PID: $($webcamPublisher.Id)"
Write-Host "MP4 publisher watchdog PID: $($filePublisher.Id)"
Write-Host "Stream 1: $CameraRtspUrl"
Write-Host "Streams 2-$StreamCount`: $FileRtspUrl"
Write-Host "Face detection: $FaceDetection"
Write-Host "Player command:"
Write-Host "  `"$playerExe`" $quotedPlayerArgs"

if ($StartPlayer) {
    if (!(Test-Path -LiteralPath $playerExe)) {
        throw "Player executable not found: $playerExe"
    }

    Start-Process `
        -FilePath $playerExe `
        -ArgumentList $playerArgs `
        -WorkingDirectory $repoRoot
}
