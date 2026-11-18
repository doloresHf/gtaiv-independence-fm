$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]

function Await-Op($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    return $netTask.Result
}

$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1'
})[0]

function Await-ActionProgress($WinRtAction, $ProgressType) {
    $asTask = $asTaskAction.MakeGenericMethod($ProgressType)
    $netTask = $asTask.Invoke($null, @($WinRtAction))
    $netTask.Wait(-1) | Out-Null
}

[Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.MediaProperties.AudioEncodingQuality, Windows.Media, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Transcoding.MediaTranscoder, Windows.Media, ContentType = WindowsRuntime] | Out-Null

function Convert-WavToMp3([string]$WavPath, [string]$Mp3Path) {
    if (Test-Path -LiteralPath $Mp3Path) {
        Remove-Item -LiteralPath $Mp3Path -Force
    }
    $outDir = Split-Path -Parent $Mp3Path
    $outName = Split-Path -Leaf $Mp3Path

    $inFile = Await-Op ([Windows.Storage.StorageFile]::GetFileFromPathAsync($WavPath)) ([Windows.Storage.StorageFile])
    $folder = Await-Op ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($outDir)) ([Windows.Storage.StorageFolder])
    $outFile = Await-Op ($folder.CreateFileAsync($outName, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])

    $profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateMp3([Windows.Media.MediaProperties.AudioEncodingQuality]::High)
    $transcoder = New-Object Windows.Media.Transcoding.MediaTranscoder
    $prep = Await-Op ($transcoder.PrepareFileTranscodeAsync($inFile, $outFile, $profile)) ([Windows.Media.Transcoding.PrepareTranscodeResult])
    if (-not $prep.CanTranscode) {
        throw "MediaTranscoder cannot transcode $WavPath : $($prep.FailureReason)"
    }
    Await-ActionProgress ($prep.TranscodeAsync()) ([double])
    Write-Host "Encoded MP3: $Mp3Path ($((Get-Item -LiteralPath $Mp3Path).Length) bytes)"
}

$assetsDir = (Join-Path $PSScriptRoot '../assets')
Convert-WavToMp3 (Join-Path $assetsDir 'IV_Probe_Target_45s.wav') (Join-Path $assetsDir 'IV_Probe_Target_45s_raw.mp3')
Convert-WavToMp3 (Join-Path $assetsDir 'IV_Probe_Control_45s.wav') (Join-Path $assetsDir 'IV_Probe_Control_45s_raw.mp3')
