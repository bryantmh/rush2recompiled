# Links Rush2Recompiled.exe into another folder (e.g. tmp\run) when build\'s exe is held by a running game, and
# copies what the game needs to run portable beside it the first time (dlls, configs, ROMs, saves, caches).
# Compile first (cmake --build build; only the final link fails), then:
#   powershell -File tools\link_to.ps1 tmp\ai_run
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'
$out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $repo $Out }
New-Item -ItemType Directory -Force $out | Out-Null
$ninja = (Get-Command ninja).Source
Push-Location $build
try {
    $cmd = & $ninja -t commands Rush2Recompiled.exe | Select-Object -Last 1
    $exe = Join-Path $out 'Rush2Recompiled.exe'
    $cmd = $cmd.Replace('/out:Rush2Recompiled.exe', "/out:`"$exe`"").Replace('/implib:Rush2Recompiled.lib', "/implib:`"$out\Rush2Recompiled.lib`"").Replace('/pdb:Rush2Recompiled.pdb', "/pdb:`"$out\Rush2Recompiled.pdb`"")
    $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    Set-Content -Encoding ascii (Join-Path $out 'link.cmd') "@set `"PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%`"`r`n@call `"$vcvars`" >nul`r`n$cmd"
    cmd /c (Join-Path $out 'link.cmd')
    if ($LASTEXITCODE -ne 0) { throw "link failed" }
    if (-not (Test-Path (Join-Path $out 'portable.txt'))) {
        Get-ChildItem $build -File | Where-Object { $_.Extension -in '.dll', '.json', '.z64', '.pak', '.txt' } | Copy-Item -Destination $out
        foreach ($d in 'saves', 'assets', 'track_cache', 'stripe_cache', 'mod_config') {
            if (Test-Path (Join-Path $build $d)) { Copy-Item -Recurse -Force (Join-Path $build $d) $out }
        }
    }
    Write-Output "linked $exe"
}
finally {
    Pop-Location
}
