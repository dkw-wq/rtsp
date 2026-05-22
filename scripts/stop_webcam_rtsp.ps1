param(
    [string]$RtspUrl = "rtsp://127.0.0.1:8554/webcam"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$logDir = Join-Path $workspaceRoot "mediamtx\logs"
$pidFiles = @(
    (Join-Path $logDir "ffmpeg-webcam.pid"),
    (Join-Path $logDir "ffmpeg-webcam2.pid"),
    (Join-Path $logDir "ffmpeg-audio.pid"),
    (Join-Path $logDir "mediamtx.pid")
)

$stoppedCount = 0

foreach ($pidFile in $pidFiles) {
    if (Test-Path $pidFile) {
        $pidValue = Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pidValue -and (Get-Process -Id $pidValue -ErrorAction SilentlyContinue)) {
            Stop-Process -Id $pidValue -Force
            $stoppedCount++
        }
        Remove-Item $pidFile -Force
    }
}

$mediaMtxExe = Join-Path (Join-Path $workspaceRoot "mediamtx") "mediamtx.exe"
$mediaMtxProcesses = Get-Process mediamtx -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $mediaMtxExe }

foreach ($process in $mediaMtxProcesses) {
    Stop-Process -Id $process.Id -Force
    $stoppedCount++
}

$ffmpegProcesses = Get-CimInstance Win32_Process -Filter "name = 'ffmpeg.exe'" -ErrorAction SilentlyContinue |
    Where-Object {
        $_.CommandLine -like "*$RtspUrl*" -or
        $_.CommandLine -like "*rtsp://127.0.0.1:8554/webcam2*" -or
        $_.CommandLine -like "*rtsp://127.0.0.1:8554/audio*"
    }

foreach ($process in $ffmpegProcesses) {
    Stop-Process -Id $process.ProcessId -Force
    $stoppedCount++
}

$watchdogProcesses = Get-CimInstance Win32_Process -Filter "name = 'powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object {
        $_.CommandLine -like "*watch_webcam_publisher.ps1*" -and (
            $_.CommandLine -like "*$RtspUrl*" -or
            $_.CommandLine -like "*rtsp://127.0.0.1:8554/webcam2*" -or
            $_.CommandLine -like "*rtsp://127.0.0.1:8554/audio*"
        )
    }

foreach ($process in $watchdogProcesses) {
    Stop-Process -Id $process.ProcessId -Force
    $stoppedCount++
}

Write-Host "Webcam RTSP stream stopped. Processes stopped: $stoppedCount"
