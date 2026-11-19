param(
    [string]$CMake = 'cmake',
    [string]$Python = 'python',
    [string]$OutputDirectory,
    [string]$Version = 'v0.3.0-beta.1'
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with $LASTEXITCODE" }
}
function Get-PeMachineTag([string]$Path) {
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) { throw "Not a valid PE file: $Path" }
        $fs.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $br.ReadInt32()
        $fs.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        if ($br.ReadUInt32() -ne 0x00004550) { throw "Missing PE signature: $Path" }
        $machine = $br.ReadUInt16()
        if ($machine -eq 0x014C) { return 'x86' }
        if ($machine -eq 0x8664) { return 'x64' }
        return ('0x' + $machine.ToString('x4'))
    } finally {
        $fs.Dispose()
    }
}
$build = Join-Path $PSScriptRoot 'build'
$output = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $build 'release' }
$archiveName = "GTAIV-Independence-FM-$Version-win64.zip"
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
if ((Get-PeMachineTag $asi) -ne 'x86') { throw "Expected x86 PE for IVRadioProbe.asi" }
if ((Get-PeMachineTag $launcher) -ne 'x64') { throw "Expected x64 PE for iv_apple_music_radio.exe" }
if ((Get-PeMachineTag $setup) -ne 'x64') { throw "Expected x64 PE for Setup.exe" }
Invoke-Checked (Join-Path $win 'ipc_protocol_selftest.exe') @('--producer-exe',$producer)
Invoke-Checked $Python @('tools/gen_stage_b_wav.py')
Invoke-Checked (Join-Path $win 'wmf_selftest.exe') @($asi,(Join-Path $PSScriptRoot 'assets'),'--producer-exe',$producer)
Invoke-Checked (Join-Path $x64 'stage_d_offline_selftest.exe') @()
Invoke-Checked (Join-Path $x64 'stage_d_bridge_selftest.exe') @('--write-sample-telemetry',(Join-Path $build 'd.jsonl'))
Invoke-Checked $Python @('tools/analyze_stage_d_telemetry.py',(Join-Path $build 'd.jsonl'),'--expect-ppm','200','--ppm-tolerance','15','--require-clean-shutdown','--max-underruns','0','--max-overruns','0')
Invoke-Checked (Join-Path $x64 'stage_e_offline_selftest.exe') @('--launcher-exe',$launcher,'--write-sample-telemetry',(Join-Path $build 'e.jsonl'))
Invoke-Checked $Python @('tools/analyze_stage_d_telemetry.py',(Join-Path $build 'e.jsonl'),'--expect-ppm','0','--ppm-tolerance','5','--require-clean-shutdown','--max-underruns','0','--max-overruns','0')
$setupTest = Start-Process -FilePath $setup -ArgumentList @("--selftest", ('"' + $build + '"')) -WindowStyle Hidden -Wait -PassThru
if ($setupTest.ExitCode -ne 0) { throw "Setup sandbox self-test failed (exit=$($setupTest.ExitCode))" }
$staging = Join-Path $build ('package-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $staging 'payload'
New-Item -ItemType Directory -Path $payload,$output -Force | Out-Null
$carriers = Join-Path $staging 'carriers-generated'
Invoke-Checked $Python @('tools/gen_stage_d_carriers.py','--out-dir',$carriers,'--silent')
foreach ($index in '01','02','03') {
    Copy-Item -LiteralPath (Join-Path $carriers "IV_Radio_Stream_$index.mp3") -Destination $payload
}
Copy-Item -LiteralPath $launcher,$asi,(Join-Path $PSScriptRoot 'release/IVRadioProbe.ini') -Destination $payload
Copy-Item -LiteralPath $setup,'QUICKSTART.md','QUICKSTART-zh-CN.md','CHANGELOG.md','LICENSE' -Destination $staging
Copy-Item -LiteralPath 'third_party/minhook/LICENSE.txt' -Destination (Join-Path $staging 'THIRD-PARTY-NOTICES.txt')
$forbidden = Get-ChildItem -LiteralPath $staging -Recurse -File | Where-Object { $_.Extension -in '.sys','.cat','.inf' }
if ($forbidden) { throw "Route A guard: proprietary driver file found in release staging: $($forbidden.FullName -join ', ')" }
$zipInputs = @('Setup.exe','QUICKSTART.md','QUICKSTART-zh-CN.md','CHANGELOG.md','LICENSE','THIRD-PARTY-NOTICES.txt','payload') | ForEach-Object { Join-Path $staging $_ }
Compress-Archive -LiteralPath $zipInputs -DestinationPath $zip -CompressionLevel Optimal
$zipItem = Get-Item -LiteralPath $zip
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $archiveName" | Set-Content -LiteralPath "$zip.sha256" -Encoding Ascii
$manifestFiles = [ordered]@{}
foreach ($f in (Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName)) {
    if ($f.FullName.StartsWith($carriers, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    $rel = $f.FullName.Substring($staging.Length + 1).Replace('\','/')
    $entry = [ordered]@{
        bytes = $f.Length
        sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($f.Extension -in '.exe','.asi') {
        $entry['pe'] = Get-PeMachineTag $f.FullName
    }
    $manifestFiles[$rel] = $entry
}
$manifest = [ordered]@{
    version = $Version
    route = 'Route-A-No-Proprietary-Drivers'
    zip_name = $archiveName
    zip_bytes = $zipItem.Length
    zip_sha256 = $hash
    files = $manifestFiles
}
$manifestPath = Join-Path $output 'candidate-manifest.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "$zip  $hash ($($zipItem.Length) bytes)"
