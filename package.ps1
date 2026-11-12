$ErrorActionPreference = "Stop"
Set-Location C:\development\CGCall

$objdump = "C:\Qt\Tools\llvm-mingw2217_64\bin\llvm-objdump.exe"

function Check($what) {
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# 1. Latest code from GitHub, then a release build. CMake already places the
#    Qt DLLs/plugins and the Hamlib/FFTW/libusb DLLs beside the exe.
git pull;                        Check "git pull"
cmake --preset release;          Check "cmake configure"
cmake --build --preset release;  Check "cmake build"

# 2. Sanity check: the build folder must already be self-contained
$build = "build\release"
foreach ($need in "JS8Call.exe", "Qt6Core.dll", "Qt6Widgets.dll", "platforms\qwindows.dll") {
    if (-not (Test-Path "$build\$need")) {
        throw "$need is missing from $build. Check that JS8_DLL_DIR was found and windeployqt ran."
    }
}

# 3. Fresh staging folder named after the commit
$hash = git rev-parse --short HEAD
$pkg  = "C:\development\CGCall-$hash"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }

# 4. Copy the runtime files only, leaving out build-system clutter
#    (JS8Call is the old windeployqt subfolder from earlier builds)
robocopy $build $pkg /E `
    /XD CMakeFiles .cmake .qt JS8Call JS8Call_autogen Testing `
    /XF *.ninja .ninja_deps .ninja_log *.cmake CMakeCache.txt *.rsp *.a compile_commands.json js8call_config.h `
    /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed (exit code $LASTEXITCODE)" }

# 5. Companion program, if it has been built
if (Test-Path companion\dist) {
    New-Item -ItemType Directory "$pkg\companion" -Force | Out-Null
    Copy-Item companion\dist\* "$pkg\companion" -Recurse -Force
}

# 6. Dependency check: every DLL that the exe and bundled DLLs import must be
#    in the package or in System32 (which a clean PC also has)
if (Test-Path $objdump) {
    $sys     = "$env:WINDIR\System32"
    $missing = @()
    Get-ChildItem $pkg -Recurse -Include *.exe, *.dll |
        Where-Object { $_.FullName -notlike "*\companion\*" } |
        ForEach-Object {
            & $objdump -p $_.FullName 2>$null | Select-String 'DLL Name:\s*(\S+)' | ForEach-Object {
                $d = $_.Matches[0].Groups[1].Value
                if ($d -notmatch '^(api-ms-|ext-ms-)' -and
                    -not (Test-Path "$pkg\$d") -and
                    -not (Test-Path "$sys\$d")) { $missing += $d }
            }
        }
    $missing = $missing | Sort-Object -Unique
    if ($missing) {
        throw "Missing DLLs: $($missing -join ', '). Add them to the DLL folder or the build."
    }
    Write-Host "Dependency check passed."
} else {
    Write-Warning "llvm-objdump not found; skipped dependency check. Test on another PC."
}

# 7. Zip
$zip = "$pkg.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $pkg -DestinationPath $zip
Write-Host "Done: $zip"
