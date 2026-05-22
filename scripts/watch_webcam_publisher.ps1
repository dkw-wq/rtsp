param(
    [Parameter(Mandatory = $true)][ValidateSet("video", "audio")][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Url,
    [Parameter(Mandatory = $true)][string]$LogDir,
    [Parameter(Mandatory = $true)][string]$LogPrefix,
    [int]$Framerate = 30,
    [string]$Size = "1280x720",
    [string]$AudioDeviceName = "",
    [switch]$DisableAudio,
    [switch]$DebugLog,
    [int]$RestartDelaySeconds = 2
)

$ErrorActionPreference = "Continue"

function New-LogArgs {
    if ($DebugLog) {
        return "-hide_banner -stats -stats_period 1 -loglevel info "
    }
    return "-hide_banner -nostats -loglevel warning "
}

function New-VideoArgs {
    $inputName = "video=$Name"
    if (!$DisableAudio -and ![string]::IsNullOrWhiteSpace($AudioDeviceName)) {
        $inputName = "${inputName}:audio=$AudioDeviceName"
    }

    $audioArgs = if ($DisableAudio -or [string]::IsNullOrWhiteSpace($AudioDeviceName)) {
        "-an"
    } else {
        "-c:a aac -ar 48000 -ac 2 -b:a 128k"
    }

    return (New-LogArgs) +
        "-f dshow -rtbufsize 100M -video_size $Size -framerate $Framerate -vcodec mjpeg " +
        "-i `"$inputName`" " +
        "-c:v libx264 -preset ultrafast -tune zerolatency -g $Framerate -pix_fmt yuv420p " +
        "$audioArgs " +
        "-f rtsp -rtsp_transport tcp $Url"
}

function New-AudioArgs {
    return (New-LogArgs) +
        "-f dshow -rtbufsize 10M " +
        "-i `"audio=$Name`" " +
        "-c:a aac -ar 48000 -ac 2 -b:a 128k " +
        "-f rtsp -rtsp_transport tcp $Url"
}

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stdoutLog = Join-Path $LogDir "$LogPrefix.stdout.log"
$stderrLog = Join-Path $LogDir "$LogPrefix.stderr.log"
$watchLog = Join-Path $LogDir "$LogPrefix.watchdog.log"

while ($true) {
    $startedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
    Add-Content -Path $watchLog -Value "[$startedAt] starting $Mode publisher: $Url"

    $ffmpegArgs = if ($Mode -eq "audio") {
        New-AudioArgs
    } else {
        New-VideoArgs
    }

    try {
        $process = Start-Process `
            -FilePath "ffmpeg" `
            -ArgumentList $ffmpegArgs `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog `
            -WindowStyle Hidden `
            -PassThru

        Add-Content -Path $watchLog -Value "[$startedAt] ffmpeg pid: $($process.Id)"
        Wait-Process -Id $process.Id
        $exitCode = $process.ExitCode
        $endedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
        Add-Content -Path $watchLog -Value "[$endedAt] ffmpeg exited with code $exitCode; restarting in ${RestartDelaySeconds}s"
    } catch {
        $failedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
        Add-Content -Path $watchLog -Value "[$failedAt] failed to run ffmpeg: $($_.Exception.Message)"
    }

    Start-Sleep -Seconds ([Math]::Max($RestartDelaySeconds, 1))
}
