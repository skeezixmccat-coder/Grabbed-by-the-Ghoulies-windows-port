# Resolve local media through the launcher; never copy game content into source.
$project=Split-Path -Parent $PSScriptRoot
$settings=Join-Path $project 'launcher-settings.json'
if(!$env:GH_ISO_PATH){
    if(!(Test-Path -LiteralPath $settings)){throw 'Open Ghoulies.exe and select your own supported disc image first.'}
    $env:GH_ISO_PATH=(Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).IsoPath
}
if(!(Test-Path -LiteralPath $env:GH_ISO_PATH -PathType Leaf)){throw 'The configured game image is missing.'}
$launcher=Join-Path $project 'Ghoulies.exe'
if(!(Test-Path -LiteralPath $launcher)){& (Join-Path $PSScriptRoot 'build_launcher.ps1') | Out-Host}
$cache=Join-Path $project 'native-storage\launcher-cache'
$p=Start-Process -FilePath $launcher -ArgumentList @('--prepare-disc',('"'+$env:GH_ISO_PATH+'"'),('"'+$cache+'"')) -PassThru -Wait -WindowStyle Hidden
if($p.ExitCode -ne 0){throw 'Disc validation failed. See %TEMP%\ghoulies-launcher-error.txt.'}
$script:NativeXbe=Join-Path $cache 'default.xbe'
