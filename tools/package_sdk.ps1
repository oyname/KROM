param(
    [string]$BuildDirectory,
    [string]$ArtifactDirectory,
    [string]$JoltSource,
    [string]$MiniaudioSource,
    [int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
$sourceDirectory = Split-Path -Parent $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $sourceDirectory 'out/build/sdk-Release' }
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $sourceDirectory 'out/sdk' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)

if (!$env:VSCMD_VER) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 or Build Tools with the C++ workload.' }
    $installation = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installation) { throw 'No supported Visual Studio 2022 C++ installation found.' }
    $setup = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $env:COMSPEC
    $startInfo.Arguments = '/d /u /v:off /s /c ""%KROM_PACKAGE_VS_SETUP%" -arch=x64 -host_arch=x64 >nul && set"'
    $startInfo.EnvironmentVariables['KROM_PACKAGE_VS_SETUP'] = $setup
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::Unicode
    $setupProcess = [Diagnostics.Process]::Start($startInfo)
    $environmentText = $setupProcess.StandardOutput.ReadToEnd()
    $setupError = $setupProcess.StandardError.ReadToEnd()
    $setupProcess.WaitForExit()
    if ($setupProcess.ExitCode) { throw "VS environment setup failed: $setupError" }
    foreach ($line in ($environmentText -split "`r?`n")) {
        $equals = $line.IndexOf('=')
        if ($equals -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $equals), $line.Substring($equals + 1), 'Process') }
    }
}
$cmake = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
if (!(Test-Path -LiteralPath $ninja)) { $ninja = (Get-Command ninja -ErrorAction Stop).Source }
function Invoke-CMake([string[]]$Arguments) {
    & $cmake @Arguments
    if ($LASTEXITCODE) { throw "CMake failed (exit $LASTEXITCODE)" }
}
$configure = @('-S', $sourceDirectory, '-B', $BuildDirectory, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    '-DKROM_BUILD_SDK=ON', "-DCMAKE_MAKE_PROGRAM=$ninja")
if ($JoltSource) { $configure += "-DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=$([IO.Path]::GetFullPath($JoltSource))" }
if ($MiniaudioSource) { $configure += "-DFETCHCONTENT_SOURCE_DIR_MINIAUDIO=$([IO.Path]::GetFullPath($MiniaudioSource))" }
if ($JoltSource -and $MiniaudioSource) { $configure += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' }
Invoke-CMake $configure
Invoke-CMake @('--build', $BuildDirectory, '--target', 'krom_sdk', '--parallel', "$Parallel")
$manifest = Get-Content -LiteralPath (Join-Path $BuildDirectory 'sdk/krom-sdk.json') -Raw | ConvertFrom-Json
$packageName = 'Krom-windows-x64-' + $manifest.sdkBuildId.Substring(0, 12)
New-Item -ItemType Directory -Path $ArtifactDirectory -Force | Out-Null
$destination = Join-Path $ArtifactDirectory $packageName
if (Test-Path -LiteralPath $destination) { throw "Package destination already exists: $destination. Choose a new ArtifactDirectory." }
Invoke-CMake @('--install', $BuildDirectory, '--config', 'Release', '--prefix', $destination)
Push-Location -LiteralPath $ArtifactDirectory
try { Invoke-CMake @('-E', 'tar', 'cf', "$packageName.zip", '--format=zip', $packageName) }
finally { Pop-Location }
Write-Output "SDK directory: $destination"
Write-Output "SDK archive: $(Join-Path $ArtifactDirectory "$packageName.zip")"
