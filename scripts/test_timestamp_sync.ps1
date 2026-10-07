param(
    [ValidateRange(1, 65535)][int]$Port = 18554,
    [string]$Config = "Release",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$mediaMtxExe = Join-Path (Split-Path -Parent $repoRoot) "mediamtx\mediamtx.exe"
$buildDir = Join-Path $repoRoot "build-vcpkg"
$probeExe = Join-Path $buildDir "bin\$Config\rtsp_sync_probe.exe"
$testDir = Join-Path $buildDir "sync-integration"
$ffmpegExe = (Get-Command ffmpeg -ErrorAction Stop).Source
if (!(Test-Path -LiteralPath $mediaMtxExe)) {
    throw "MediaMTX not found: $mediaMtxExe"
}
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Test port $Port is already in use; choose a different -Port."
}
if (!$SkipBuild) {
    & cmake --build $buildDir --config $Config --target rtsp_sync_probe
    if ($LASTEXITCODE -ne 0) { throw "Probe build failed" }
}
if (!(Test-Path -LiteralPath $probeExe)) {
    throw "Probe executable not found: $probeExe"
}
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
$configPath = Join-Path $testDir "mediamtx.yml"
@"
logLevel: info
rtspAddress: 127.0.0.1:$Port
rtspTransports: [tcp]
rtmp: no
hls: no
webrtc: no
srt: no
paths:
  all_others:
"@ | Set-Content -LiteralPath $configPath -Encoding ascii

$started = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
try {
    $server = Start-Process -FilePath $mediaMtxExe -ArgumentList ('"' + $configPath + '"') `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $testDir "mediamtx.stdout.log") `
        -RedirectStandardError (Join-Path $testDir "mediamtx.stderr.log")
    $started.Add($server)
    Start-Sleep -Seconds 1
    if ($server.HasExited) { throw "Isolated MediaMTX failed; see $testDir" }

    $urls = @("rtsp://127.0.0.1:$Port/video", "rtsp://127.0.0.1:$Port/usb", "rtsp://127.0.0.1:$Port/audio")
    $inputs = @("testsrc2=size=320x240:rate=15", "color=blue:size=320x240:rate=15", "sine=frequency=440:sample_rate=44100")
    for ($index = 0; $index -lt 3; ++$index) {
        $codecArgs = if ($index -eq 2) {
            "-vn -c:a aac -ar 44100 -ac 2"
        } else {
            "-an -c:v libx264 -preset ultrafast -tune zerolatency -g 15"
        }
        $publisher = Start-Process -FilePath $ffmpegExe `
            -ArgumentList "-hide_banner -nostats -loglevel warning -re -f lavfi -i $($inputs[$index]) -t 60 $codecArgs -f rtsp -rtsp_transport tcp $($urls[$index])" `
            -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $testDir "publisher-$index.stdout.log") `
            -RedirectStandardError (Join-Path $testDir "publisher-$index.stderr.log")
        $started.Add($publisher)
        # Deliberately give the three publishers different startup times and PTS origins.
        Start-Sleep -Milliseconds 600
        if ($publisher.HasExited) { throw "Publisher $index failed; see $testDir" }
    }
    & $probeExe @urls
    if ($LASTEXITCODE -ne 0) { throw "Timestamp synchronization probe failed; see $testDir" }
}
finally {
    foreach ($process in $started) {
        if (!$process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(3000) | Out-Null
        }
        $process.Dispose()
    }
}
