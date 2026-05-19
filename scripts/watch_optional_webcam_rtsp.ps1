param(
    [string]$CameraName = "Logi C270 HD WebCam",
    [string]$RtspUrl = "rtsp://127.0.0.1:8554/webcam2",
    [string]$VideoSize = "1280x720",
    [int]$Framerate = 30,
    [int]$PollSeconds = 3,
    [switch]$FfmpegDebug
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $repoRoot
$mediaMtxDir = Join-Path $workspaceRoot "mediamtx"
$logDir = Join-Path $mediaMtxDir "logs"
$pidFile = Join-Path $logDir "ffmpeg-webcam2.pid"
$stderrLog = Join-Path $logDir "ffmpeg-webcam2.stderr.log"
$stdoutLog = Join-Path $logDir "ffmpeg-webcam2.stdout.log"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Get-DShowVideoDevices {
    $previousErrorActionPreference = $ErrorActionPreference
    $previousNativeErrorPreference = $null
    $hasNativeErrorPreference = Test-Path Variable:\PSNativeCommandUseErrorActionPreference
    if ($hasNativeErrorPreference) {
        $previousNativeErrorPreference = $PSNativeCommandUseErrorActionPreference
    }

    try {
        $ErrorActionPreference = "Continue"
        if ($hasNativeErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $false
        }
        $output = & ffmpeg -hide_banner -list_devices true -f dshow -i dummy 2>&1
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        if ($hasNativeErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativeErrorPreference
        }
    }

    $devices = @()
    foreach ($line in $output) {
        $text = $line.ToString()
        if ($text -match '"([^"]+)"\s+\(video\)') {
            $devices += $Matches[1]
        }
    }

    return $devices | Select-Object -Unique
}

function Test-DShowVideoDevice {
    param(
        [Parameter(Mandatory = $true)][string]$Name
    )

    $devices = Get-DShowVideoDevices
    return [bool]($devices | Where-Object { $_ -eq $Name } | Select-Object -First 1)
}

function Get-PublisherProcess {
    if (!(Test-Path -LiteralPath $pidFile)) {
        return $null
    }

    $pidValue = Get-Content -LiteralPath $pidFile -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (!$pidValue) {
        Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        return $null
    }

    $process = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
    if (!$process) {
        Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    }
    return $process
}

function Start-SecondPublisher {
    $logArgs = if ($FfmpegDebug) {
        "-hide_banner -stats -stats_period 1 -loglevel info "
    } else {
        "-hide_banner -nostats -loglevel warning "
    }

    $ffmpegArgs = $logArgs +
        "-f dshow -rtbufsize 100M -video_size $VideoSize -framerate $Framerate -vcodec mjpeg " +
        "-i `"video=$CameraName`" " +
        "-c:v libx264 -preset ultrafast -tune zerolatency -g $Framerate -pix_fmt yuv420p " +
        "-an -f rtsp -rtsp_transport tcp $RtspUrl"

    $process = Start-Process `
        -FilePath "ffmpeg" `
        -ArgumentList $ffmpegArgs `
        -WorkingDirectory $mediaMtxDir `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -WindowStyle Hidden `
        -PassThru

    Set-Content -LiteralPath $pidFile -Value $process.Id
    return $process
}

$missingLogged = $false
while ($true) {
    $publisher = Get-PublisherProcess
    $devicePresent = Test-DShowVideoDevice -Name $CameraName

    if (!$devicePresent) {
        if ($publisher) {
            Stop-Process -Id $publisher.Id -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        }
        if (!$missingLogged) {
            Write-Output "Optional camera '$CameraName' is not present; waiting."
            $missingLogged = $true
        }
        Start-Sleep -Seconds $PollSeconds
        continue
    }

    $missingLogged = $false
    if (!$publisher) {
        Write-Output "Starting optional camera publisher: $CameraName -> $RtspUrl"
        $publisher = Start-SecondPublisher
        Start-Sleep -Seconds 2
        if ($publisher.HasExited) {
            Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue |
                Select-Object -Last 20
            Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        }
    }

    Start-Sleep -Seconds $PollSeconds
}
