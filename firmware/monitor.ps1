$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Resolve-FirstExistingPath {
    param([string[]]$Candidates)
    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path $Candidate)) { return $Candidate }
    }
    return $null
}

$DefaultIdfPath = "C:\Espressif\v5.5.4\esp-idf"
$DefaultPythonEnv = Join-Path $env:USERPROFILE ".espressif\python_env\idf5.5_py3.11_env"

$IdfPath = Resolve-FirstExistingPath @($env:IDF_PATH, $DefaultIdfPath)
if (-not $IdfPath) {
    throw "ESP-IDF not found. Please set IDF_PATH or update monitor.ps1 for your local environment."
}

$PythonExe = Resolve-FirstExistingPath @(
    $(if ($env:IDF_PYTHON_ENV_PATH) { Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe" }),
    $(Join-Path $DefaultPythonEnv "Scripts\python.exe")
)

if (-not $PythonExe) {
    $PythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($PythonCommand) {
        $PythonExe = $PythonCommand.Source
    } else {
        throw "ESP-IDF Python environment not found. Please set IDF_PYTHON_ENV_PATH or ensure python is available in PATH."
    }
}

$PythonEnvPath = if ($env:IDF_PYTHON_ENV_PATH) {
    $env:IDF_PYTHON_ENV_PATH
} else {
    Split-Path (Split-Path $PythonExe -Parent) -Parent
}

$env:IDF_PATH = $IdfPath
$env:IDF_PYTHON_ENV_PATH = $PythonEnvPath
$ExtraPaths = @()
if (Test-Path (Join-Path $PythonEnvPath "Scripts")) { $ExtraPaths += Join-Path $PythonEnvPath "Scripts" }
if (Test-Path (Join-Path $IdfPath "tools")) { $ExtraPaths += Join-Path $IdfPath "tools" }
if ($ExtraPaths.Count -gt 0) {
    $env:PATH = (($ExtraPaths + $env:PATH.Split(";")) | Where-Object { $_ } | Select-Object -Unique) -join ";"
}

Push-Location $ProjectRoot
Write-Host "--- 正在进入日志监控 (按 Ctrl+] 退出) ---" -ForegroundColor Cyan
& $PythonExe "$IdfPath\tools\idf.py" monitor
Pop-Location
