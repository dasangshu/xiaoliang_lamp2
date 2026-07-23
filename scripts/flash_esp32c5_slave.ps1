# Build & flash ESP32-C5 esp-hosted SDIO slave for P4 host (xiaoliang)
# Usage: .\scripts\flash_esp32c5_slave.ps1 -Port COMx
param(
    [Parameter(Mandatory = $true)]
    [string]$Port
)

$ErrorActionPreference = "Stop"
$Repo = "$env:TEMP\esp-hosted-mcu"
$Branch = "release/2.7"
$SlaveDir = Join-Path $Repo "esp_hosted_slave"
$Defaults = Join-Path $PSScriptRoot "..\main\boards\xiaoliang\sdkconfig.slave.defaults"

if (-not (Test-Path $Repo)) {
    git clone --depth 1 -b $Branch https://github.com/espressif/esp-hosted-mcu.git $Repo
}

. "F:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1"
Set-Location $SlaveDir
idf.py set-target esp32c5
if (Test-Path $Defaults) {
    Copy-Item $Defaults "sdkconfig.defaults" -Force
}
idf.py -p $Port build flash monitor
