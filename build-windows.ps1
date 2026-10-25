# csb-3ds: build on Windows with Docker Desktop.
# From a PowerShell prompt in this folder:
#   powershell -ExecutionPolicy Bypass -File .\build-windows.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "Docker Desktop is not installed or not on PATH. Install it, reboot, start it, then re-run." -ForegroundColor Red
    exit 1
}

# Make sure the Docker engine is actually running (Docker Desktop must be open).
docker info *> $null
if ($LASTEXITCODE -ne 0) {
    $dd = "$Env:ProgramFiles\Docker\Docker\Docker Desktop.exe"
    if (Test-Path $dd) {
        Write-Host "== Docker engine not running - starting Docker Desktop..."
        Start-Process $dd
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Seconds 5
            docker info *> $null
            if ($LASTEXITCODE -eq 0) { break }
            Write-Host "   waiting for the engine ($(($i + 1) * 5)s)"
        }
    }
    docker info *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Docker engine still isn't running. Open Docker Desktop, wait for 'Engine running', then re-run." -ForegroundColor Red
        Write-Host "If Docker Desktop reports a WSL problem, run:  wsl --update   and restart Docker Desktop."
        exit 1
    }
}

Write-Host "== pulling devkitpro/devkitarm (first run downloads ~1 GB)"
docker pull devkitpro/devkitarm
if ($LASTEXITCODE -ne 0) { Write-Host "docker pull failed" -ForegroundColor Red; exit 1 }

Write-Host "== building"
docker run --rm -v "${PWD}:/work" -w /work devkitpro/devkitarm bash -c "sed -i 's/\r$//' build.sh && bash build.sh"
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed - copy the output above into the chat." -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "Built: $PWD\out\csb3ds.3dsx" -ForegroundColor Green
Write-Host "Copy out\csb3ds.3dsx and out\csb.cfg to SD:\3ds\csb3ds\ along with GRAPHICS.DAT and DUNGEON.DAT"
