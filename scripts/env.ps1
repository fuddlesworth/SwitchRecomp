# Puts the toolchain that scripts\bootstrap.ps1 downloaded on PATH for this PowerShell window, and sets CC, CXX and AR
# to the compiler it was set up with. Dot-source it:
#
#   . scripts\env.ps1
#
# -Isolated replaces PATH with only those tools, Git and Windows' own folders (bootstrap.ps1 uses it, so that nothing
# else installed on the machine is picked up by accident).
param([switch]$Isolated)

$repo = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo 'deps\tools'
$compilerFile = Join-Path $tools 'compiler.txt'
if (-not (Test-Path $compilerFile)) {
    throw 'Run scripts\bootstrap.ps1 first: deps\tools is not set up.'
}
$compiler = (Get-Content $compilerFile -Raw).Trim()

$paths = @((Join-Path $tools 'python'), (Join-Path $tools 'nasm-3.02'))
if ($compiler -eq 'clang') {
    # Before w64devkit, so that ar is llvm-ar, matching the compiler
    $paths += Join-Path $tools 'llvm-mingw-20261006-ucrt-x86_64\bin'
    $env:CC = 'clang'; $env:CXX = 'clang++'; $env:AR = 'llvm-ar'
} else {
    $env:CC = 'gcc'; $env:CXX = 'g++'; $env:AR = 'ar'
}
$paths += Join-Path $tools 'w64devkit\bin'

if ($Isolated) {
    $git = Split-Path -Parent (Get-Command git).Source
    $system = Join-Path $env:SystemRoot 'System32'
    $env:PATH = ($paths + @($git, $system, $env:SystemRoot, (Join-Path $system 'WindowsPowerShell\v1.0'))) -join ';'
} else {
    $env:PATH = ($paths -join ';') + ';' + $env:PATH
}
Write-Host "SwitchRecomp tools ($compiler) are on PATH"
