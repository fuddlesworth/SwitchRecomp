# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# Sets up a Windows machine for SwitchRecomp with nothing installed: downloads the toolchain into deps\tools, each file
# pinned and checked against its SHA-256. The third-party libraries come with the CMake build (cmake\dependencies.cmake).
#
#   powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
#
# Afterwards, `. scripts\env.ps1` puts the tools on PATH in a PowerShell window.
#
#   llvm-mingw 20261006   Clang, lld and the MinGW-w64 headers and libraries
#   CMake 4.4.4, Ninja 1.13.2
#   NASM 3.02             FFmpeg's assembly
#   w64devkit 2.10.0      only its sh (busybox) and GNU make, which FFmpeg's configure and Makefiles need
#   Python 3.15.0         switchrecomp and the scripts (the embeddable distribution)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is many times slower with its progress bar

$repo = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo 'deps\tools'
$downloads = Join-Path $tools 'downloads'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

$packages = @(
    @{ Name = 'llvm-mingw'; Dir = 'llvm-mingw-20261006-ucrt-x86_64'; Kind = 'zip'
       Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip'
       Sha256 = '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a' },
    @{ Name = 'CMake'; Dir = 'cmake-4.4.4-windows-x86_64'; Kind = 'zip'
       Url = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'
       Sha256 = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536' },
    @{ Name = 'Ninja'; Dir = 'ninja-1.13.2'; Kind = 'zip-flat'
       Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
       Sha256 = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65' },
    @{ Name = 'w64devkit'; Dir = 'w64devkit'; Kind = 'sfx'
       Url = 'https://github.com/skeeto/w64devkit/releases/download/v2.10.0/w64devkit-x64-2.10.0.7z.exe'
       Sha256 = '18d0a4c71a166f8401ab6305781bec5882b40b5e06ba9807c61cb5f3b3c6325e' },
    @{ Name = 'NASM'; Dir = 'nasm-3.02'; Kind = 'zip'
       Url = 'https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip'
       Sha256 = '161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7' },
    @{ Name = 'Python'; Dir = 'python'; Kind = 'zip-flat'
       Url = 'https://www.python.org/ftp/python/3.15.0/python-3.15.0-embed-amd64.zip'
       Sha256 = 'a6999bcbcbe56f937159ef79c2630260eca99f65f7860bc5cd20430324079a44' }
)

# Windows 10 and 11 ship bsdtar, which unpacks zip files much faster than Expand-Archive
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'

foreach ($p in $packages) {
    $dest = Join-Path $tools $p.Dir
    $marker = Join-Path $dest '.sha256'
    if ((Test-Path $marker) -and ((Get-Content $marker -Raw).Trim() -eq $p.Sha256)) {
        Write-Host "$($p.Name) is up to date"
        continue
    }
    $file = Join-Path $downloads ([IO.Path]::GetFileName($p.Url))
    if (-not (Test-Path $file) -or (Get-FileHash -Algorithm SHA256 $file).Hash -ne $p.Sha256) {
        Write-Host "Downloading $($p.Name)"
        Invoke-WebRequest -Uri $p.Url -OutFile $file -UseBasicParsing
    }
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash
    if ($hash -ne $p.Sha256) {
        Remove-Item $file
        throw "$($p.Name): the download's SHA-256 is $hash, expected $($p.Sha256). Not using it."
    }
    Write-Host "Unpacking $($p.Name)"
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    switch ($p.Kind) {
        'sfx' {
            # A 7-Zip self-extracting archive: -o is the folder it unpacks into, -y answers yes to everything
            $proc = Start-Process -FilePath $file -ArgumentList '-y', "-o$tools" -Wait -PassThru -NoNewWindow
            if ($proc.ExitCode -ne 0) { throw "$($p.Name) did not unpack (exit $($proc.ExitCode))" }
        }
        'zip' { & $tar -xf $file -C $tools; if ($LASTEXITCODE) { throw "$($p.Name) did not unpack" } }
        'zip-flat' {
            New-Item -ItemType Directory -Force -Path $dest | Out-Null
            & $tar -xf $file -C $dest; if ($LASTEXITCODE) { throw "$($p.Name) did not unpack" }
        }
    }
    if (-not (Test-Path $dest)) { throw "$($p.Name) did not unpack into $dest" }
    Set-Content -Path $marker -Value $p.Sha256
}
# Toolchains from before (w64devkit) are no longer used
Remove-Item -ErrorAction SilentlyContinue (Join-Path $tools 'compiler.txt')

Write-Host 'Done. In a new PowerShell window, run `. scripts\env.ps1` first, then build with `cmake --preset default`.'
