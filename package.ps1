$ErrorActionPreference = "Stop"

$Repo  = "C:\development\CGCall"
$Build = "$Repo\build\release"
$Pkg   = "C:\development\CGCall-0.1"
$Zip   = "C:\development\CGCall-0.1.zip"
$Extra = @("$Repo\companion")      # files/folders to ship next to the exe

# 1. Stop any running copy so files aren't locked
Get-Process CGCall, JS8Call -ErrorAction SilentlyContinue | Stop-Process -Force

# 2. Build (this also runs windeployqt and the DLL copy)
cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# 3. Fresh package folder, never reuse the old one
Remove-Item $Pkg -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $Build $Pkg -Recurse
foreach ($e in $Extra) { Copy-Item $e $Pkg -Recurse -Force }

# 4. Strip leftovers and build junk
Remove-Item "$Pkg\JS8Call.exe" -Force -ErrorAction SilentlyContinue
Get-ChildItem $Pkg -Recurse -Include CMakeFiles, *.cmake, CMakeCache.txt, *.ninja, .ninja_* |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# 5. Verify before zipping
$exes = Get-ChildItem $Pkg -Filter *.exe
if ($exes.Name -notcontains "CGCall.exe") { throw "CGCall.exe missing" }
if ($exes.Name -contains "JS8Call.exe")   { throw "JS8Call.exe still present" }
foreach ($dll in "Qt6Core","Qt6Gui","Qt6Widgets","Qt6Multimedia","Qt6SerialPort") {
    if (-not (Test-Path "$Pkg\$dll.dll")) { throw "Missing $dll.dll" }
}

# 6. Zip
Remove-Item $Zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $Pkg -DestinationPath $Zip
Write-Host "Done: $Zip"
