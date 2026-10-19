# Project2DFX Zeneric Edition - PC bootstrap for Windows PowerShell 5.1+
# This file is invoked by build.bat. It supports both Git checkouts and ZIP downloads.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildDebug = $false
$Update = $false
$GenerateOnly = $false
$ShowHelp = $false

foreach ($option in $args) {
    switch ($option.ToLowerInvariant()) {
        '--debug'         { $BuildDebug = $true }
        '--update'        { $Update = $true }
        '--latest'        { $Update = $true }
        '--generate-only' { $GenerateOnly = $true }
        '--help'          { $ShowHelp = $true }
        '-h'              { $ShowHelp = $true }
        default { Write-Host "Unknown option: $option" -ForegroundColor Red; exit 2 }
    }
}
if ($ShowHelp) {
    Write-Host 'Usage: build.bat [--update] [--debug] [--generate-only] [--help]'
    Write-Host '  default: download missing PC dependencies, generate VS2026 solution, build Release Win32'
    Write-Host '  --update: fetch latest dependency commits (may break upstream compatibility)'
    Write-Host '  --debug: build Debug instead of Release'
    Write-Host '  --generate-only: restore dependencies and generate solution, skip MSBuild'
    exit 0
}

function Invoke-Git {
    param([string[]]$GitArgs)
    & git.exe @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "Git command failed (exit $LASTEXITCODE): git $($GitArgs -join ' ')" }
}

function Restore-ZipDependency {
    param([hashtable]$Dependency)
    $dir = Join-Path $ProjectRoot $Dependency.Path
    $marker = Join-Path $dir $Dependency.Marker
    $repoPresent = Test-Path -LiteralPath (Join-Path $dir '.git')
    if (-not $repoPresent) {
        if (Test-Path -LiteralPath $dir) {
            # Never delete or overwrite a non-empty user directory.
            $existing = @(Get-ChildItem -LiteralPath $dir -Force)
            if ($existing.Count -gt 0) {
                if (Test-Path -LiteralPath $marker) {
                    Write-Host "  Reusing existing $($Dependency.Path) (unmanaged files; not updating)"
                    return
                }
                throw "$($Dependency.Path) contains files but has no Git metadata or required '$($Dependency.Marker)'. Back it up/remove that directory, then rerun."
            }
        }
        Write-Host "  Downloading $($Dependency.Path) (and its nested submodules)..."
        Invoke-Git -GitArgs @('clone', '--recurse-submodules', $Dependency.Url, $dir)
    } elseif ($Update) {
        # Do not overwrite local edits: ff-only refuses divergent local histories.
        Write-Host "  Updating $($Dependency.Path)..."
        Invoke-Git -GitArgs @('-C', $dir, 'pull', '--ff-only')
    } else {
        Write-Host "  Found $($Dependency.Path)"
    }

    # SafetyHook and other nested dependencies inside injector need recursion.
    Invoke-Git -GitArgs @('-C', $dir, 'submodule', 'update', '--init', '--recursive')
    if (-not (Test-Path -LiteralPath $marker)) {
        throw "$($Dependency.Path) was fetched but '$($Dependency.Marker)' is missing. Check your connection / dependency version."
    }
}

try {
    if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot 'premake5.lua'))) {
        throw 'premake5.lua is missing. Run build.bat from the extracted project root.'
    }
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        throw 'Git for Windows is required to restore dependencies. Install it from https://git-scm.com/download/win and reopen PowerShell.'
    }

    # Detect accidental double UTF-8 BOMs before C++ compilation. MSVC reads the
    # second BOM as U+FEFF in source text and reports misleading syntax errors.
    $sourceDir = Join-Path $ProjectRoot 'source'
    Get-ChildItem -LiteralPath $sourceDir -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.c', '.ixx', '.h', '.hpp') } |
        ForEach-Object {
            $stream = [System.IO.File]::OpenRead($_.FullName)
            try {
                $header = New-Object byte[] 6
                if ($stream.Read($header, 0, 6) -eq 6 -and
                    $header[0] -eq 239 -and $header[1] -eq 187 -and $header[2] -eq 191 -and
                    $header[3] -eq 239 -and $header[4] -eq 187 -and $header[5] -eq 191) {
                    throw "Double UTF-8 BOM detected: $($_.FullName). Save this file as UTF-8 with at most one BOM."
                }
            } finally { $stream.Dispose() }
        }

    Write-Host '[1/3] Restoring PC dependencies...'
    # Only the five PC dependencies. ps2sdk and pspsdk are not required for Win32.
    $dependencies = @(
        @{ Path='external/hooking'; Url='https://github.com/ThirteenAG/Hooking.Patterns.git'; Marker='Hooking.Patterns.h' },
        @{ Path='external/IniReader'; Url='https://github.com/ThirteenAG/IniReader.git'; Marker='IniReader.h' },
        @{ Path='external/injector'; Url='https://github.com/ThirteenAG/injector.git'; Marker='include' },
        @{ Path='external/filewatch'; Url='https://github.com/ThomasMonkman/filewatch.git'; Marker='FileWatch.hpp' },
        @{ Path='external/ModUtils'; Url='https://github.com/CookiePLMonster/ModUtils.git'; Marker='README.md' }
    )
    if (Test-Path -LiteralPath (Join-Path $ProjectRoot '.git')) {
        # Actual Git checkout: use its recorded, reproducible submodule revisions.
        # --update intentionally opts into advancing their upstream versions.
        $gitArgs = @('-C', $ProjectRoot, 'submodule', 'update', '--init', '--recursive')
        if ($Update) { $gitArgs += '--remote' }
        $gitArgs += @($dependencies | ForEach-Object { $_.Path })
        Invoke-Git -GitArgs $gitArgs
    } else {
        # Downloaded ZIP: there is no parent Git history, so clone dependencies directly.
        $externalRoot = Join-Path $ProjectRoot 'external'
        if (-not (Test-Path -LiteralPath $externalRoot)) {
            New-Item -ItemType Directory -Path $externalRoot -Force | Out-Null
        }
        foreach ($dependency in $dependencies) { Restore-ZipDependency $dependency }
    }
    foreach ($dependency in $dependencies) {
        $marker = Join-Path $ProjectRoot (Join-Path $dependency.Path $dependency.Marker)
        if (-not (Test-Path -LiteralPath $marker)) {
            throw "Missing dependency file: $marker"
        }
    }
    foreach ($nested in @('external/injector/safetyhook/include', 'external/injector/zydis')) {
        if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $nested))) {
            throw "Missing nested injector submodule: $nested"
        }
    }

    Write-Host '[2/3] Generating Visual Studio 2026 solution...'
    $premake = Join-Path $ProjectRoot 'premake5.exe'
    if (-not (Test-Path -LiteralPath $premake)) {
        throw 'premake5.exe is missing from this ZIP. Obtain a trusted Premake 5 build supporting vs2026.'
    }
    Push-Location -LiteralPath $ProjectRoot
    try {
        & $premake 'vs2026'
        if ($LASTEXITCODE -ne 0) { throw "Premake vs2026 generation failed (exit $LASTEXITCODE)." }
    } finally { Pop-Location }

    $solution = @(
        (Join-Path $ProjectRoot 'build\III.VC.SA.IV.Project2DFX.slnx'),
        (Join-Path $ProjectRoot 'build\III.VC.SA.IV.Project2DFX.sln')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $solution) {
        throw 'Premake completed without creating build\III.VC.SA.IV.Project2DFX.slnx or .sln.'
    }
    if ($GenerateOnly) {
        Write-Host "Generated $solution" -ForegroundColor Green
        exit 0
    }

    Write-Host '[3/3] Building the PC solution (Win32)...'
    if (-not ${env:ProgramFiles(x86)}) { throw 'Could not find Program Files (x86); VS2026 requires 64-bit Windows.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Visual Studio vswhere.exe was not found. Install Visual Studio 2026 with Desktop development with C++.'
    }
    # VS 2026 = major version 18. Prefer its MSBuild, not older VS installations.
    $msbuild = & $vswhere '-latest' '-version' '[18.0,19.0)' '-products' '*' '-requires' 'Microsoft.Component.MSBuild' '-find' 'MSBuild\**\Bin\MSBuild.exe' |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $msbuild) {
        throw 'VS2026 MSBuild not found. Install the Desktop development with C++ workload and Windows SDK.'
    }
    $config = if ($BuildDebug) { 'Debug' } else { 'Release' }
    Write-Host "Using: $msbuild"
    Write-Host "Solution: $solution ($config, Win32)"
    & $msbuild $solution '/m' "/p:Configuration=$config" '/p:Platform=Win32' '/restore' '/verbosity:minimal'
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed (exit $LASTEXITCODE). See the compiler errors above." }

    Write-Host 'Build succeeded. Outputs are in the data\<project>\ folders.' -ForegroundColor Green
    exit 0
} catch {
    Write-Host ''
    Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
