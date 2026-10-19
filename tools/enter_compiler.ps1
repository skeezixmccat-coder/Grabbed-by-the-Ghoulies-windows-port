param([ValidateSet('amd64','x86')][string]$Arch='amd64')
$ErrorActionPreference='Stop'
# Repeated official developer-shell setup can duplicate process-local paths
# until Windows command creation exceeds its limit. Preserve each unique entry.
function Remove-CompilerPathDuplicates {
    foreach($name in @('PATH','INCLUDE','LIB','LIBPATH')) {
        $value=[Environment]::GetEnvironmentVariable($name,'Process')
        if($value){[Environment]::SetEnvironmentVariable($name,(($value -split ';' | Select-Object -Unique) -join ';'),'Process')}
    }
}
Remove-CompilerPathDuplicates
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere)){throw 'Install Visual Studio Build Tools with Desktop development with C++ and a Windows SDK.'}
$installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$installation){throw 'No installed Visual Studio C++ toolchain was found.'}
$devshell=Join-Path $installation 'Common7\Tools\Launch-VsDevShell.ps1'
& $devshell -SkipAutomaticLocation -Arch $Arch -HostArch amd64
Remove-CompilerPathDuplicates
