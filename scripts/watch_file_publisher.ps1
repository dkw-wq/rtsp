param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Url,
    [Parameter(Mandatory = $true)][string]$LogDir,
    [Parameter(Mandatory = $true)][string]$LogPrefix,
    [switch]$Transcode,
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

function New-FileArgs {
    $videoArgs = if ($Transcode) {
        "-c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p "
    } else {
        "-c:v copy "
    }

    return (New-LogArgs) +
        "-re -stream_loop -1 -i `"$Path`" " +
        "-map 0:v:0 -an " +
        $videoArgs +
        "-f rtsp -rtsp_transport tcp $Url"
}

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stdoutLog = Join-Path $LogDir "$LogPrefix.stdout.log"
$stderrLog = Join-Path $LogDir "$LogPrefix.stderr.log"
$watchLog = Join-Path $LogDir "$LogPrefix.watchdog.log"

while ($true) {
    $startedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
    Add-Content -Path $watchLog -Value "[$startedAt] starting file publisher: $Path -> $Url"

    try {
        $process = Start-Process `
            -FilePath "ffmpeg" `
            -ArgumentList (New-FileArgs) `
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
