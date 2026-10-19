# Builds the N1 native launcher and runtime (32-bit, MSVC). Output: build\native\
param([switch]$Run,[int]$Timeout=30)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$out=Join-Path $project 'build\native'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& (Join-Path $PSScriptRoot 'enter_compiler.ps1') -Arch x86
Push-Location $out
try {
    & cl /nologo /W4 /O1 /GS- /Oi- /c (Join-Path '..\..' 'native\launcher\launcher.c') /Fo:launcher.obj
    if($LASTEXITCODE -ne 0){throw 'launcher compile failed'}
    & link /nologo launcher.obj kernel32.lib /OUT:ghoulies-native.exe /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /ENTRY:LauncherEntry /SUBSYSTEM:CONSOLE /NODEFAULTLIB
    if($LASTEXITCODE -ne 0){throw 'launcher link failed'}
    # Relative compiler inputs keep private checkout paths out of __FILE__ strings.
    $gl=Join-Path '..\..' 'native\runtime\nv2a_gl'
    $sources=@((Join-Path '..\..' 'native\runtime\ghoulies_runtime.cpp'),(Join-Path $gl 'nv2a_gl.cpp'),(Join-Path $gl 'nv2a_shaders.cpp'),(Join-Path $gl 'nv2a_vsh.cpp'),(Join-Path $gl 'nv2a_psh.cpp'),(Join-Path $gl 'swizzle.cpp'))
    & cl /nologo /W3 /O2 /std:c++20 /EHsc /MT /LD /wd4244 /wd4267 /wd4018 /wd4305 /wd4996 /D_CRT_SECURE_NO_WARNINGS $sources /Fe:ghoulies_runtime.dll /link /MAP:ghoulies_runtime.map /BASE:0x10000000 /DYNAMICBASE:NO /LARGEADDRESSAWARE opengl32.lib gdi32.lib user32.lib
    if($LASTEXITCODE -ne 0){throw 'runtime build failed'}
    foreach($f in @('ghoulies-native.exe','ghoulies_runtime.dll')){ Write-Output ("{0} {1}" -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant(),$f) }
    # Retain the exact source/binary relationship even when build and launch
    # happen in separate commands. Later source edits cannot rewrite this.
    $manifestFiles=@(Get-ChildItem (Join-Path $project 'native\runtime') -File -Recurse) + @(
        Get-Item (Join-Path $project 'native\launcher\launcher.c'),
                 (Join-Path $project 'include\kernel_exports.hpp'), $PSCommandPath,
                 (Join-Path $PSScriptRoot 'enter_compiler.ps1'))
    $sourceHashes=@($manifestFiles | Sort-Object FullName | ForEach-Object { Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 } | Select-Object Path,Hash)
    $binaryHashes=@(Get-FileHash .\ghoulies-native.exe,.\ghoulies_runtime.dll -Algorithm SHA256 | Select-Object Path,Hash)
    @{builtUtc=[DateTime]::UtcNow.ToString('o'); command='powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_native.ps1';
      sourceHashes=$sourceHashes; binaryHashes=$binaryHashes} | ConvertTo-Json -Depth 5 |
        Set-Content -Encoding UTF8 (Join-Path $out 'build-manifest.json')
    if($Run){
        . (Join-Path $PSScriptRoot 'resolve_native_media.ps1')
        $stamp=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
        $evidence=Join-Path $project ('evidence\native-n1-'+$stamp)
        New-Item -ItemType Directory -Force -Path $evidence | Out-Null
        $report=Join-Path $evidence 'report.txt'
        $sourceFiles=@(Get-ChildItem (Join-Path $project 'native\runtime') -File -Recurse) + @(
            Get-Item (Join-Path $project 'native\launcher\launcher.c'),
                     (Join-Path $project 'include\kernel_exports.hpp'),
                     (Join-Path $PSScriptRoot 'build_native.ps1'),
                     (Join-Path $PSScriptRoot 'enter_compiler.ps1'))
        $sourceFiles | Sort-Object FullName | ForEach-Object { Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 } |
            Select-Object Path,Hash | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $evidence 'source-hashes.json')
        Get-FileHash .\ghoulies-native.exe,.\ghoulies_runtime.dll -Algorithm SHA256 | Select-Object Path,Hash |
            ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $evidence 'binary-hashes.json')
        $inputScriptSource=$env:GH_INPUT_SCRIPT
        $inputScriptHash=$null
        if($inputScriptSource){
            Copy-Item -LiteralPath $inputScriptSource -Destination (Join-Path $evidence 'input-script.txt')
            $inputScriptHash=(Get-FileHash -LiteralPath (Join-Path $evidence 'input-script.txt') -Algorithm SHA256).Hash
        }
        @{
            present=$env:GH_PRESENT; keyboard=$env:GH_KEYBOARD; inputScript=$inputScriptSource; inputScriptSha256=$inputScriptHash; singleCoreHost=$env:GH_SINGLE_CORE_HOST
            build="powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_native.ps1 -Run -Timeout $Timeout"
            launcher='ghoulies-native.exe'; timeoutSeconds=$Timeout; watch=$env:GH_WATCH; gpuTrace=$env:GH_GPU_TRACE
            xbe=$NativeXbe; report=$report
            presentation=$(if($env:GH_PRESENT -eq '1'){'interactive scan-out after authentic audio'}else{'hidden WGL/window; internal capture only'})
        } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $evidence 'run.json')
        try {
            if($inputScriptSource){$env:GH_INPUT_SCRIPT=Join-Path $evidence 'input-script.txt'}
            & .\ghoulies-native.exe $NativeXbe $report $Timeout
            $nativeExit=$LASTEXITCODE
        } finally { $env:GH_INPUT_SCRIPT=$inputScriptSource }
        @{exitCode=$nativeExit; stop=(Get-Content -LiteralPath $report | Select-String '^STOP ' | Select-Object -Last 1 -ExpandProperty Line)} |
            ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $evidence 'result.json')
        Write-Output ("exit code {0}; report {1}" -f $nativeExit,$report)
    }
} finally { Pop-Location }
