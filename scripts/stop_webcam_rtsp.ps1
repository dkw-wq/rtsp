$ErrorActionPreference = "SilentlyContinue"

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$logDir = Join-Path $workspaceRoot "mediamtx\logs"
$pidFiles = @(
    Join-Path $logDir "ffmpeg-webcam.pid",
    Join-Path $logDir "mediamtx.pid"
)

foreach ($pidFile in $pidFiles) {
    if (Test-Path $pidFile) {
        $pidValue = Get-Content $pidFile | Select-Object -First 1
        if ($pidValue) {
            Stop-Process -Id $pidValue -Force
        }
        Remove-Item $pidFile -Force
    }
}

Write-Host "Webcam RTSP stream stopped."
