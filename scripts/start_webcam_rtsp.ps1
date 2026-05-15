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
    $inVideoSection = $false

    foreach ($line in $output) {
        $text = $line.ToString()
        if ($text -match '"([^"]+)"\s+\(video\)') {
            $devices += $Matches[1]
            continue
        }
        if ($text -match "DirectShow video devices") {
            $inVideoSection = $true
            continue
        }
        if ($text -match "DirectShow audio devices") {
            $inVideoSection = $false
            continue
        }
        if ($inVideoSection -and $text -match '"([^"]+)"') {
            $devices += $Matches[1]
        }
    }

    return $devices | Select-Object -Unique
}

function Test-DShowVideoDevice {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$Devices
    )

    return [bool]($Devices | Where-Object { $_ -eq $Name } | Select-Object -First 1)
}

$dualEnabled = $Dual
if ($Dual) {
    $videoDevices = Get-DShowVideoDevices
    if (!(Test-DShowVideoDevice -Name $SecondCameraName -Devices $videoDevices)) {
        Write-Warning "Second camera '$SecondCameraName' was not found. Falling back to single-camera RTSP publishing."
        if ($videoDevices.Count -gt 0) {
            Write-Host "Available DirectShow video devices:"
            foreach ($device in $videoDevices) {
                Write-Host "  - $device"
            }
        }
        $dualEnabled = $false
    }
}

$ffmpegPidFiles = @(
    (Join-Path $logDir "ffmpeg-webcam.pid")
)
if ($dualEnabled) {
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
            Write-Host "FFmpeg publisher is already running from PID file: $ffmpegPidFile"
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

    $inputName = "video=$Name"
    if (!$DisableAudio -and ![string]::IsNullOrWhiteSpace($AudioDeviceName)) {
        $inputName = "${inputName}:audio=$AudioDeviceName"
    }

    $audioArgs = if ($DisableAudio -or [string]::IsNullOrWhiteSpace($AudioDeviceName)) {
        "-an"
    } else {
        "-c:a aac -ar 48000 -ac 2 -b:a 128k"
    }

    $logArgs = if ($DebugLog) {
        "-hide_banner -stats -stats_period 1 -loglevel info "
    } else {
        "-hide_banner -nostats -loglevel warning "
    }

    $ffmpegArgs = $logArgs +
        "-f dshow -rtbufsize 100M -video_size $Size -framerate $Framerate -vcodec mjpeg " +
        "-i `"$inputName`" " +
        "-c:v libx264 -preset ultrafast -tune zerolatency -g $Framerate -pix_fmt yuv420p " +
        "$audioArgs " +
        "-f rtsp -rtsp_transport tcp $Url"

    $ffmpegProcess = Start-Process `
        -FilePath "ffmpeg" `
        -ArgumentList $ffmpegArgs `
        -WorkingDirectory $mediaMtxDir `
        -RedirectStandardOutput (Join-Path $logDir "$LogPrefix.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "$LogPrefix.stderr.log") `
        -WindowStyle Hidden `
        -PassThru

    Set-Content -Path $PidFile -Value $ffmpegProcess.Id
    return $ffmpegProcess
}

function Start-AudioPublisher {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$PidFile,
        [Parameter(Mandatory = $true)][string]$LogPrefix,
        [switch]$DebugLog
    )

    $logArgs = if ($DebugLog) {
        "-hide_banner -stats -stats_period 1 -loglevel info "
    } else {
        "-hide_banner -nostats -loglevel warning "
    }

    $ffmpegArgs = $logArgs +
        "-f dshow -rtbufsize 10M " +
        "-i `"audio=$Name`" " +
        "-c:a aac -ar 48000 -ac 2 -b:a 128k " +
        "-f rtsp -rtsp_transport tcp $Url"

    $ffmpegProcess = Start-Process `
        -FilePath "ffmpeg" `
        -ArgumentList $ffmpegArgs `
        -WorkingDirectory $mediaMtxDir `
        -RedirectStandardOutput (Join-Path $logDir "$LogPrefix.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "$LogPrefix.stderr.log") `
        -WindowStyle Hidden `
        -PassThru

    Set-Content -Path $PidFile -Value $ffmpegProcess.Id
    return $ffmpegProcess
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
        -DisableAudio:($NoAudio -or $dualEnabled) `
        -DebugLog:$FfmpegDebug
    Url = $RtspUrl
    Log = Join-Path $logDir "ffmpeg-webcam.stderr.log"
}

if ($dualEnabled) {
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

Start-Sleep -Seconds 3

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
        throw "FFmpeg failed to publish webcam stream: $($publisher.Url)"
    }
}

Write-Host "MediaMTX PID: $($mediaMtxProcess.Id)"
foreach ($publisher in $publishers) {
    Write-Host "FFmpeg PID: $($publisher.Process.Id)"
    Write-Host "RTSP URL: $($publisher.Url)"
}

if ($dualEnabled) {
    Write-Host "Player config rtsp_urls:"
    Write-Host "  - `"$RtspUrl`""
    Write-Host "  - `"$SecondRtspUrl`""
    if (!$NoAudio -and ![string]::IsNullOrWhiteSpace($AudioName)) {
        Write-Host "Player config audio_rtsp_url: `"$AudioRtspUrl`""
    }
} else {
    Write-Host "Player command:"
    Write-Host "  .\build-vcpkg\bin\Release\rtsp_player.exe `"$RtspUrl`""
}
