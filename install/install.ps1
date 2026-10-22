# Install saturn on Windows 10/11.
#   irm https://raw.githubusercontent.com/phanminhtai1029/saturn/main/install/install.ps1 | iex
# Downloads the latest release into %LOCALAPPDATA%\Programs\saturn and adds it to your user PATH.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repo = 'phanminhtai1029/saturn'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\saturn'
$exe = Join-Path $dir 'saturn.exe'
$url = "https://github.com/$repo/releases/latest/download/saturn-windows-x86_64.exe"

New-Item -ItemType Directory -Force -Path $dir | Out-Null
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $exe -UseBasicParsing
Unblock-File -Path $exe

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not $userPath) { $userPath = '' }
if (-not (($userPath -split ';') -contains $dir)) {
    $newPath = (($userPath.TrimEnd(';')), $dir | Where-Object { $_ }) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    Write-Host "Added $dir to your user PATH."
}
if (-not (($env:Path -split ';') -contains $dir)) { $env:Path = "$env:Path;$dir" }

Write-Host "Installed: $exe"
Write-Host "Run it with: saturn   (open a new terminal window if the command is not found)"
