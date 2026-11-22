param(
    [Parameter(Mandatory=$true)][string]$SdkDirectory,
    [Parameter(Mandatory=$true)][string]$SmokeExecutable,
    [switch]$HideSources,
    [switch]$ReadOnly,
    [switch]$StartupOnly
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$sdk = (Resolve-Path -LiteralPath $SdkDirectory).Path
$probe = (Resolve-Path -LiteralPath $SmokeExecutable).Path
# This test mutates permissions and moves source folders: operate only on an
# installed validation copy inside this workspace, never on an arbitrary SDK.
$workspacePrefix = $workspace.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if (!$sdk.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Validation SDK must be inside the test workspace.' }
$manifest = Get-Content -LiteralPath (Join-Path $sdk 'krom-sdk.json') -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 2) { throw 'Expected a complete v2 SDK.' }
foreach ($config in Get-ChildItem -LiteralPath (Join-Path $sdk 'cmake') -Filter '*.cmake') {
    $text = Get-Content -LiteralPath $config.FullName -Raw
    if ($text.Contains($workspace.Replace('\','/')) -or $text.Contains($workspace.Replace('/','\'))) {
        throw "SDK CMake file retains source/build path: $($config.Name)"
    }
}
$testRoot = Join-Path $workspace ('out/sdk-acceptance Größe-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$sdkProbe = Join-Path $sdk 'bin/krom_sdk_smoke.exe'
Copy-Item -LiteralPath $probe -Destination $sdkProbe
$before = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" })
$originalAcl = $null
$aclSaved = $false
$moved = New-Object 'Collections.Generic.List[string]'
$hidden = Join-Path $testRoot 'hidden-source'
$savedAppData = $env:LOCALAPPDATA
$savedSdkEnvironment = @{}
foreach ($name in @('VULKAN_SDK','DXC_PATH','PATH','HTTP_PROXY','HTTPS_PROXY','ALL_PROXY','NO_PROXY')) {
    $savedSdkEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:VULKAN_SDK = $null
    $env:DXC_PATH = $null
    $env:PATH = (($env:PATH -split ';') | Where-Object { $_ -notmatch 'VulkanSDK' }) -join ';'
    $env:HTTP_PROXY = 'http://127.0.0.1:9'
    $env:HTTPS_PROXY = $env:HTTP_PROXY
    $env:ALL_PROXY = $env:HTTP_PROXY
    $env:NO_PROXY = ''
    if ($ReadOnly) {
        $originalAcl = Get-Acl -LiteralPath $sdk
        # This is a disposable installation copy. Requiring normal inherited
        # permissions lets an unprivileged user restore them with /reset rather
        # than /restore (which can require SeRestore/SeSecurityPrivilege).
        foreach ($item in @((Get-Item -LiteralPath $sdk)) + @(Get-ChildItem -LiteralPath $sdk -Recurse)) {
            $acl = Get-Acl -LiteralPath $item.FullName
            if ($acl.AreAccessRulesProtected -or @($acl.Access | Where-Object { !$_.IsInherited }).Count) {
                throw 'Read-only validation requires a fresh copy with inherited permissions.'
            }
        }
        $readOnlyAcl = New-Object Security.AccessControl.DirectorySecurity
        $readOnlyAcl.SetAccessRuleProtection($true, $false)
        foreach ($entry in $originalAcl.Access) {
            if ($entry.AccessControlType -eq 'Allow') {
                $rule = [Security.AccessControl.FileSystemAccessRule]::new(
                    $entry.IdentityReference, [Security.AccessControl.FileSystemRights]::ReadAndExecute,
                    [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',
                    [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
                $readOnlyAcl.AddAccessRule($rule)
            }
        }
        $aclSaved = $true
        Set-Acl -LiteralPath $sdk -AclObject $readOnlyAcl
        try { [IO.File]::WriteAllText((Join-Path $sdk 'forbidden-write.txt'), 'unexpected'); throw 'Read-only SDK still accepted a write.' }
        catch [UnauthorizedAccessException] { Write-Output 'PASS: SDK directory denies writes' }
    }
    if ($HideSources) {
        New-Item -ItemType Directory -Path $hidden -Force | Out-Null
        foreach ($folder in @('app','include','src','addons','examples','cmake','shaders','assets','editor')) {
            $from = [IO.Path]::GetFullPath((Join-Path $workspace $folder))
            $to = [IO.Path]::GetFullPath((Join-Path $hidden $folder))
            if (!$from.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                !$to.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe source move destination.' }
            Move-Item -LiteralPath $from -Destination $to
            $moved.Add($folder)
        }
        Write-Output 'PASS: engine source and resources hidden from original paths'
    }
    $env:LOCALAPPDATA = Join-Path $testRoot 'Benutzer Größe'
    $log = Join-Path $testRoot 'sdk-project-flow.log'
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $sdkProbe
    $startInfo.Arguments = if ($StartupOnly) { '--ini-path' } else { '--sdk-project-flow' }
    $startInfo.WorkingDirectory = $testRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $sdkProcess = [Diagnostics.Process]::Start($startInfo)
    $outputTask = $sdkProcess.StandardOutput.ReadToEndAsync()
    $errorTask = $sdkProcess.StandardError.ReadToEndAsync()
    if (!$sdkProcess.WaitForExit(660000)) { $sdkProcess.Kill(); throw 'SDK validation process timed out.' }
    [IO.File]::WriteAllText($log, $outputTask.Result + [Environment]::NewLine + $errorTask.Result)
    $code = $sdkProcess.ExitCode
    Get-Content -LiteralPath $log -Tail 20
    if ($code) { Get-Content -LiteralPath $log -Tail 90; throw "SDK project flow failed ($code). Log: $log" }
    $after = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" })
    if (@(Compare-Object $before $after).Count) { throw 'SDK installation files changed during project build/export.' }
    if ($StartupOnly) { Write-Output 'PASS: installed SDK editor startup; installation unchanged' }
    else { Write-Output 'PASS: installed SDK project creation, scripts, Play, runtime and export; installation unchanged' }
    Write-Output "Validation artifacts: $testRoot"
} finally {
    $env:LOCALAPPDATA = $savedAppData
    foreach ($name in $savedSdkEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedSdkEnvironment[$name], 'Process') }
    try {
        if ($aclSaved) {
            & icacls.exe $sdk /reset /T /C /Q | Out-Null
            if ($LASTEXITCODE) { throw "Cannot restore inherited permissions on validation copy: $sdk" }
        }
    } finally {
        foreach ($folder in $moved) {
            $from = [IO.Path]::GetFullPath((Join-Path $hidden $folder))
            $to = [IO.Path]::GetFullPath((Join-Path $workspace $folder))
            if (!$from.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                !$to.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe source restore destination.' }
            Move-Item -LiteralPath $from -Destination $to
        }
    }
}
exit 0
