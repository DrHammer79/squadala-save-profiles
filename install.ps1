# Save Profiles: copies mod\mod.json, mod\save_profiles.dll and mod\fonts into the co-op game's mods\save-profiles.
#   install.cmd "C:\path\to\the kit"              install, or update after build.cmd
#   install.cmd "C:\path\to\the kit" -Uninstall   take the mod out (your saves stay where they are: README.md)
# With no kit path, the folder this one sits in is taken as the kit.
param([string]$Kit, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Kit) { $Kit = Split-Path -Parent $here }
if (-not (Test-Path -LiteralPath (Join-Path $Kit 'hookshot'))) {
    Write-Host "No co-op kit in $Kit (it has no hookshot folder)." -ForegroundColor Yellow
    Write-Host 'Run: install.cmd "C:\path\to\the kit"   (the kit''s own folder, the one with hookshot in it)'
    exit 1
}
$kit = (Resolve-Path -LiteralPath $Kit).Path

# The game this kit built (hookshot\game-folder.txt, written by the kit's build; relative to the kit when inside it)
$game = Join-Path $kit 'hookshot\game'
$note = Join-Path $kit 'hookshot\game-folder.txt'
if (Test-Path -LiteralPath $note) {
    $n = "$(Get-Content -LiteralPath $note -TotalCount 1)".Trim()
    if ($n) { $game = if ([System.IO.Path]::IsPathRooted($n)) { $n } else { Join-Path $kit $n } }
}
$exe = @((Join-Path $game 'bin\longshot.exe'), (Join-Path $game 'longshot.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $exe) {
    Write-Host "No built co-op game in $game. Start the kit once (Start Squadala.bat; Start OoT Co-op.bat on kits before 0.11.4) to build it, then run this again." -ForegroundColor Yellow
    exit 1
}
$exeDir = Split-Path -Parent $exe
$running = Get-Process longshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($exeDir, [StringComparison]::OrdinalIgnoreCase) }
if ($running) {
    Write-Host 'The co-op game is running: close it first (it loads mods when it starts).' -ForegroundColor Yellow
    exit 1
}
$dest = Join-Path $exeDir 'mods\save-profiles'

if ($Uninstall) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Write-Host "Save Profiles taken out of $dest."
    Write-Host 'Your saves were not touched: the file you played last is in the game''s save folder, the others in its perfil<N> folders (README.md).'
    exit 0
}

foreach ($f in 'mod.json', 'save_profiles.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $here "mod\$f"))) { Write-Host "mod\$f is missing: run build.cmd first." -ForegroundColor Yellow; exit 1 }
}
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -LiteralPath (Join-Path $here 'mod\mod.json') -Destination $dest -Force
Copy-Item -LiteralPath (Join-Path $here 'mod\save_profiles.dll') -Destination $dest -Force
# The letters (PT Serif Bold, Lato Bold Italic; SIL Open Font License, the OFL-*.txt beside them)
$fonts = Join-Path $here 'mod\fonts'
if (Test-Path -LiteralPath $fonts) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dest 'fonts') | Out-Null
    Get-ChildItem -LiteralPath $fonts -File | Copy-Item -Destination (Join-Path $dest 'fonts') -Force
}
Write-Host "Save Profiles installed in $dest." -ForegroundColor Green
Write-Host 'Start the game: the file select comes up before the title. The game''s log says "coop: mod save-profiles: ...".'
