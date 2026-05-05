param(
    [string]$CameraName = "HP True Vision FHD Camera",
    [string]$AudioName = "麦克风阵列 (2- 适用于数字麦克风的英特尔® 智音技术)",
    [string]$RtspUrl = "rtsp://127.0.0.1:8554/webcam",
    [string]$VideoSize = "1280x720",
    [int]$Framerate = 30,
    [switch]$NoAudio
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$mediaMtxDir = Join-Path $workspaceRoot "mediamtx"
$mediaMtxExe = Join-Path $mediaMtxDir "mediamtx.exe"
$logDir = Join-Path $mediaMtxDir "logs"
$mediaMtxPidFile = Join-Path $logDir "mediamtx.pid"
$ffmpegPidFile = Join-Path $logDir "ffmpeg-webcam.pid"

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

if (Test-Path $ffmpegPidFile) {
    $oldPid = Get-Content $ffmpegPidFile -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($oldPid -and (Get-Process -Id $oldPid -ErrorAction SilentlyContinue)) {
        Write-Host "FFmpeg webcam publisher is already running. RTSP URL: $RtspUrl"
        exit 0
    }
}

$inputName = "video=$CameraName"
if (!$NoAudio -and ![string]::IsNullOrWhiteSpace($AudioName)) {
    $inputName = "${inputName}:audio=$AudioName"
}

$audioArgs = if ($NoAudio -or [string]::IsNullOrWhiteSpace($AudioName)) {
    "-an"
} else {
    "-c:a aac -ar 48000 -ac 2 -b:a 128k"
}

$ffmpegArgs = "-hide_banner -nostats -loglevel warning " +
    "-f dshow -rtbufsize 100M -video_size $VideoSize -framerate $Framerate -vcodec mjpeg " +
    "-i `"$inputName`" " +
    "-c:v libx264 -preset ultrafast -tune zerolatency -g $Framerate -pix_fmt yuv420p " +
    "$audioArgs " +
    "-f rtsp -rtsp_transport tcp $RtspUrl"

$ffmpegProcess = Start-Process `
    -FilePath "ffmpeg" `
    -ArgumentList $ffmpegArgs `
    -WorkingDirectory $mediaMtxDir `
    -RedirectStandardOutput (Join-Path $logDir "ffmpeg-webcam.stdout.log") `
    -RedirectStandardError (Join-Path $logDir "ffmpeg-webcam.stderr.log") `
    -WindowStyle Hidden `
    -PassThru

Set-Content -Path $ffmpegPidFile -Value $ffmpegProcess.Id
Start-Sleep -Seconds 3

if ($ffmpegProcess.HasExited) {
    Get-Content (Join-Path $logDir "ffmpeg-webcam.stderr.log") -ErrorAction SilentlyContinue
    throw "FFmpeg failed to publish webcam stream."
}

Write-Host "MediaMTX PID: $($mediaMtxProcess.Id)"
Write-Host "FFmpeg PID: $($ffmpegProcess.Id)"
Write-Host "RTSP URL: $RtspUrl"
