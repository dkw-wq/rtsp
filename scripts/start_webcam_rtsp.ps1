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
$secondWatcherPidFile = Join-Path $logDir "ffmpeg-webcam2-watcher.pid"

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
}
if ($Dual -and !$NoAudio -and ![string]::IsNullOrWhiteSpace($AudioName)) {
    $ffmpegPidFiles += (Join-Path $logDir "ffmpeg-audio.pid")
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

if ($Dual -and (Test-Path $secondWatcherPidFile)) {
    $oldWatcherPid = Get-Content $secondWatcherPidFile -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($oldWatcherPid -and (Get-Process -Id $oldWatcherPid -ErrorAction SilentlyContinue)) {
        Write-Host "Optional second camera watcher is already running from PID file: $secondWatcherPidFile"
        Write-Host "Run scripts\stop_webcam_rtsp.ps1 before restarting publishers."
        exit 0
    }
    Remove-Item $secondWatcherPidFile -Force
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

function Start-OptionalSecondWatcher {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Size,
        [switch]$DebugLog
    )

    $watcherScript = Join-Path $PSScriptRoot "watch_optional_webcam_rtsp.ps1"
    if (!(Test-Path -LiteralPath $watcherScript)) {
        throw "Optional second camera watcher script not found: $watcherScript"
    }

    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$watcherScript`"",
        "-CameraName", "`"$Name`"",
        "-RtspUrl", "`"$Url`"",
        "-VideoSize", "`"$Size`"",
        "-Framerate", "$Framerate"
    )
    if ($DebugLog) {
        $arguments += "-FfmpegDebug"
    }

    $watcherProcess = Start-Process `
        -FilePath "powershell" `
        -ArgumentList ($arguments -join " ") `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput (Join-Path $logDir "ffmpeg-webcam2-watcher.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "ffmpeg-webcam2-watcher.stderr.log") `
        -WindowStyle Hidden `
        -PassThru

    Set-Content -Path $secondWatcherPidFile -Value $watcherProcess.Id
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

}

if ($Dual -and !$NoAudio -and ![string]::IsNullOrWhiteSpace($AudioName)) {
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

$secondWatcher = $null
if ($Dual -and !$dualEnabled) {
    $secondSize = if ([string]::IsNullOrWhiteSpace($SecondVideoSize)) {
        $VideoSize
    } else {
        $SecondVideoSize
    }
    $secondWatcher = Start-OptionalSecondWatcher `
        -Name $SecondCameraName `
        -Url $SecondRtspUrl `
        -Size $secondSize `
        -DebugLog:$FfmpegDebug
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
        if ($secondWatcher -and !$secondWatcher.HasExited) {
            Stop-Process -Id $secondWatcher.Id -Force
        }
        if (Test-Path $secondWatcherPidFile) {
            Remove-Item $secondWatcherPidFile -Force
        }
        throw "Publisher watchdog failed to start: $($publisher.Url)"
    }
}
if ($secondWatcher) {
    Write-Host "Optional second camera watcher PID: $($secondWatcher.Id)"
}

Write-Host "MediaMTX PID: $($mediaMtxProcess.Id)"
foreach ($publisher in $publishers) {
    Write-Host "Publisher watchdog PID: $($publisher.Process.Id)"
    Write-Host "RTSP URL: $($publisher.Url)"
}

if ($Dual) {
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
