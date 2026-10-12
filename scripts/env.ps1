# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# Puts the toolchain that scripts\bootstrap.ps1 downloaded on PATH for this PowerShell window, and sets CC and CXX to
# its Clang. Dot-source it, after allowing scripts for this window (Windows runs none by default):
#
#   Set-ExecutionPolicy -Scope Process Bypass
#   . scripts\env.ps1
#
# -Isolated replaces PATH with only those tools, Git and Windows' own folders (CI uses it, so that nothing else
# installed on the machine is picked up by accident).
param([switch]$Isolated)

$repo = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo 'deps\tools'
# bootstrap.ps1 lists the folders, in PATH order, so their names (and versions) are written in one place
$list = Join-Path $tools 'path.txt'
if (-not (Test-Path $list)) {
    throw 'Run scripts\bootstrap.ps1 first: deps\tools is not set up.'
}
$paths = @(Get-Content $list | Where-Object { $_ } | ForEach-Object { Join-Path $tools $_ })
if (-not (Test-Path (Join-Path $paths[0] 'clang.exe'))) {
    throw 'Run scripts\bootstrap.ps1 again: deps\tools has no Clang.'
}
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
