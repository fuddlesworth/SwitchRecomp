# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# Puts the toolchain that scripts\bootstrap.ps1 downloaded on PATH for this PowerShell window, and sets CC and CXX to
# its Clang. Dot-source it:
#
#   . scripts\env.ps1
#
# -Isolated replaces PATH with only those tools, Git and Windows' own folders (CI uses it, so that nothing else
# installed on the machine is picked up by accident).
param([switch]$Isolated)

$repo = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo 'deps\tools'
$clang = Join-Path $tools 'llvm-mingw-20261006-ucrt-x86_64\bin'
if (-not (Test-Path (Join-Path $clang 'clang.exe'))) {
    throw 'Run scripts\bootstrap.ps1 first: deps\tools is not set up.'
}

$paths = @($clang, (Join-Path $tools 'cmake-4.4.4-windows-x86_64\bin'), (Join-Path $tools 'ninja-1.13.2'),
           (Join-Path $tools 'nasm-3.02'), (Join-Path $tools 'python'),
           # last, so its GCC, CMake and Ninja never come first: FFmpeg's build only needs its sh and make
           (Join-Path $tools 'w64devkit\bin'))
$env:CC = 'clang'; $env:CXX = 'clang++'

if ($Isolated) {
    $system = Join-Path $env:SystemRoot 'System32'
    $paths += @($system, $env:SystemRoot, (Join-Path $system 'WindowsPowerShell\v1.0'))
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) { $paths += Split-Path -Parent $git.Source }
    $env:PATH = $paths -join ';'
} else {
    $env:PATH = ($paths -join ';') + ';' + $env:PATH
}
Write-Host 'SwitchRecomp tools (Clang) are on PATH'
