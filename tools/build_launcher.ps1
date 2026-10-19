param()
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if(!(Test-Path -LiteralPath $compiler)){throw '.NET Framework 4.x C# compiler is required (included with supported Windows).'}
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /utf8output /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Web.Extensions.dll /out:"$project\Ghoulies.exe" "$project\native\frontend\Launcher.cs" "$project\native\frontend\LauncherTests.cs"
if($LASTEXITCODE -ne 0){throw 'Launcher build failed'}
Get-FileHash -LiteralPath (Join-Path $project 'Ghoulies.exe') -Algorithm SHA256

$out=Join-Path $project 'build/native'
New-Item -ItemType Directory -Force $out | Out-Null
@{builtUtc=[DateTime]::UtcNow.ToString('o');sources=@(Get-FileHash -LiteralPath "$project/native/frontend/Launcher.cs","$project/native/frontend/LauncherTests.cs",$PSCommandPath | Select-Object Path,Hash);binary=(Get-FileHash -LiteralPath "$project/Ghoulies.exe" | Select-Object Path,Hash)} |
 ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $out 'launcher-build-manifest.json')
