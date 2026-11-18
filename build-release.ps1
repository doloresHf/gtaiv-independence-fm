param([string]$CMake = 'cmake', [string]$Python = 'python', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with $LASTEXITCODE" }
}
$build = Join-Path $PSScriptRoot 'build'
$output = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $build 'release' }
$archiveName = 'GTAIV-Independence-FM-v0.2.0-beta.1-win64.zip'
$zip = Join-Path $output $archiveName
if (Test-Path -LiteralPath $zip) { throw "Move the prior release output aside before packaging: $zip" }
foreach ($arch in 'x64','Win32') {
    $tree = Join-Path $build $arch
    Invoke-Checked $CMake @('-S',$PSScriptRoot,'-B',$tree,'-G','Visual Studio 17 2022','-A',$arch)
    Invoke-Checked $CMake @('--build',$tree,'--config','Release','--parallel','6')
}
$win = Join-Path $build 'Win32/bin/Release'
$x64 = Join-Path $build 'x64/bin/Release'
$producer = Join-Path $x64 'iv_stage_c_producer.exe'
$launcher = Join-Path $x64 'iv_apple_music_radio.exe'
$asi = Join-Path $win 'IVRadioProbe.asi'
$setup = Join-Path $x64 'Setup.exe'
Invoke-Checked (Join-Path $win 'ipc_protocol_selftest.exe') @('--producer-exe',$producer)
Invoke-Checked $Python @('tools/gen_stage_b_wav.py')
Invoke-Checked (Join-Path $win 'wmf_selftest.exe') @($asi,(Join-Path $PSScriptRoot 'assets'),'--producer-exe',$producer)
Invoke-Checked (Join-Path $x64 'stage_d_offline_selftest.exe') @()
Invoke-Checked (Join-Path $x64 'stage_d_bridge_selftest.exe') @('--write-sample-telemetry',(Join-Path $build 'd.jsonl'))
Invoke-Checked $Python @('tools/analyze_stage_d_telemetry.py',(Join-Path $build 'd.jsonl'),'--expect-ppm','200','--ppm-tolerance','15','--require-clean-shutdown','--max-underruns','0','--max-overruns','0')
Invoke-Checked (Join-Path $x64 'stage_e_offline_selftest.exe') @('--launcher-exe',$launcher,'--write-sample-telemetry',(Join-Path $build 'e.jsonl'))
Invoke-Checked $Python @('tools/analyze_stage_d_telemetry.py',(Join-Path $build 'e.jsonl'),'--expect-ppm','0','--ppm-tolerance','5','--require-clean-shutdown','--max-underruns','0','--max-overruns','0')
$setupTest = Start-Process -FilePath $setup -ArgumentList @("--selftest", ('"' + $build + '"')) -WindowStyle Hidden -Wait -PassThru
if ($setupTest.ExitCode -ne 0) { throw "Setup sandbox self-test failed" }
$staging = Join-Path $build ('package-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $staging 'payload'
New-Item -ItemType Directory -Path $payload,$output -Force | Out-Null
$carriers = Join-Path $staging 'carriers-generated'
Invoke-Checked $Python @('tools/gen_stage_d_carriers.py','--out-dir',$carriers,'--silent')
foreach ($index in '01','02','03') {
    Copy-Item -LiteralPath (Join-Path $carriers "IV_Radio_Stream_$index.mp3") -Destination $payload
}
Copy-Item -LiteralPath $launcher,$asi,(Join-Path $PSScriptRoot 'release/IVRadioProbe.ini') -Destination $payload
Copy-Item -LiteralPath $setup,'QUICKSTART.md','QUICKSTART-zh-CN.md','LICENSE' -Destination $staging
Copy-Item -LiteralPath 'third_party/minhook/LICENSE.txt' -Destination (Join-Path $staging 'THIRD-PARTY-NOTICES.txt')
$zipInputs = @('Setup.exe','QUICKSTART.md','QUICKSTART-zh-CN.md','LICENSE','THIRD-PARTY-NOTICES.txt','payload') | ForEach-Object { Join-Path $staging $_ }
Compress-Archive -LiteralPath $zipInputs -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $archiveName" | Set-Content -LiteralPath "$zip.sha256" -Encoding Ascii
Write-Host "$zip  $hash"
