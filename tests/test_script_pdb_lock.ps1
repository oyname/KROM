param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [string]$Target = 'krom_scripts_zweitesProjekt'
)
$ErrorActionPreference = 'Stop'
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')
function CacheValue([string]$key) {
    $line = $cache | Where-Object { $_ -match ('^' + [regex]::Escape($key) + ':[^=]+=') } | Select-Object -First 1
    if (!$line) { throw "Missing CMake cache entry: $key" }
    return $line.Substring($line.IndexOf('=') + 1)
}
$ninja = CacheValue 'CMAKE_MAKE_PROGRAM'
$compiler = CacheValue 'CMAKE_CXX_COMPILER'
$match = [regex]::Match($compiler, '^(.+)[\\/]VC[\\/]Tools[\\/]MSVC[\\/]')
if (!$match.Success) { throw 'This test requires an MSVC/Ninja build.' }
$vsDev = Join-Path $match.Groups[1].Value 'Common7/Tools/VsDevCmd.bat'
$binDir = Join-Path $BuildDir 'bin'
$entryObject = Join-Path $BuildDir ("CMakeFiles/$Target.dir/addons/script/ScriptModuleEntry.cpp.obj")
if (!(Test-Path -LiteralPath $entryObject)) { throw "Missing module object: $entryObject" }
$held = [Collections.Generic.List[IO.FileStream]]::new()
$names = [Collections.Generic.HashSet[string]]::new()
try {
    # Sperre die feste alte PDB und alle bisherigen Generationen gegen Schreiben.
    foreach ($file in Get-ChildItem -LiteralPath $binDir -Filter ($Target + '*.pdb')) {
        $held.Add([IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
        [void]$names.Add($file.Name)
    }
    if (!$held.Count) { throw 'Build the script module once before running this test.' }
    for ($attempt = 1; $attempt -le 2; ++$attempt) {
        # Nur ein Build-Artefakt als neuer markieren: erzwingt den Link, ohne
        # Projektquellen zu editieren oder Engine-Libraries neu zu bauen.
        (Get-Item -LiteralPath $entryObject).LastWriteTimeUtc = [DateTime]::UtcNow
        $command = '"' + $vsDev + '" -arch=x64 -host_arch=x64 >nul && "' + $ninja +
            '" -C "' + $BuildDir + '" ' + $Target
        & $env:ComSpec /d /c $command
        if ($LASTEXITCODE -ne 0) { throw "Link attempt $attempt failed with locked PDBs." }
        $newFiles = @(Get-ChildItem -LiteralPath $binDir -Filter ($Target + '_*.pdb') |
            Where-Object { !$names.Contains($_.Name) })
        if ($newFiles.Count -ne 1) { throw "Expected one new PDB, got $($newFiles.Count)." }
        $file = $newFiles[0]
        $held.Add([IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
        [void]$names.Add($file.Name)
        Write-Output "PASS: link $attempt succeeded with old PDBs locked; $($file.Name)"
    }
}
finally {
    foreach ($stream in $held) { $stream.Dispose() }
}
