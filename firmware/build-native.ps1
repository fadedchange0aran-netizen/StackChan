$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-RunLog {
    param($LogFile, $Message)
    $Line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    if ($LogFile) { Add-Content -Path $LogFile -Value $Line }
    Write-Host $Line
}

function Publish-LatestLog {
    param($SourcePath, $DestinationPath)
    if (Test-Path $SourcePath) { Copy-Item -Path $SourcePath -Destination $DestinationPath -Force }
}

function Merge-LogFiles {
    param($DestinationPath, $SourcePaths)
    $Merged = @()
    foreach ($SourcePath in $SourcePaths) {
        if (Test-Path $SourcePath) { $Merged += Get-Content -Path $SourcePath }
    }
    if ($Merged.Count -gt 0) { Set-Content -Path $DestinationPath -Value $Merged }
    else { Set-Content -Path $DestinationPath -Value "No log content was captured." }
}

function Invoke-NativeLogged {
    param($FilePath, $ArgumentList, $LogFile, $WorkingDirectory, $RunLogFile)
    if (-not $WorkingDirectory) { $WorkingDirectory = $PWD.Path }
    $StdOutLog = "$LogFile.stdout.log"
    $StdErrLog = "$LogFile.stderr.log"
    foreach ($f in $StdOutLog, $StdErrLog, $LogFile) { if (Test-Path $f) { Remove-Item -Force $f } }
    if ($RunLogFile) { Write-RunLog -LogFile $RunLogFile -Message ("Running: {0} {1}" -f $FilePath, ($ArgumentList -join " ")) }
    try {
        $Process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -WorkingDirectory $WorkingDirectory -RedirectStandardOutput $StdOutLog -RedirectStandardError $StdErrLog -NoNewWindow -PassThru -Wait
        $Merged = @()
        if (Test-Path $StdOutLog) { $Merged += Get-Content -Path $StdOutLog }
        if (Test-Path $StdErrLog) { $Merged += Get-Content -Path $StdErrLog }
        if ($Merged.Count -gt 0) { Set-Content -Path $LogFile -Value $Merged }
        else { New-Item -ItemType File -Path $LogFile -Force | Out-Null }
        return $Process.ExitCode
    } catch {
        if ($RunLogFile) { Write-RunLog -LogFile $RunLogFile -Message $_.Exception.Message }
        throw
    }
}

function Repair-GitSubmoduleState {
    param($RepoRoot, $SubmodulePath)
    $NormalizedSubmodulePath = $SubmodulePath -replace "/", "\"
    $ModuleGitDir = Join-Path $RepoRoot ".git\modules\$NormalizedSubmodulePath"
    $WorktreeDir = Join-Path $RepoRoot $NormalizedSubmodulePath
    if (Test-Path (Join-Path $ModuleGitDir "shallow.lock")) { Remove-Item -Force (Join-Path $ModuleGitDir "shallow.lock") }
    if (Test-Path $ModuleGitDir) { Remove-Item -Recurse -Force $ModuleGitDir }
    if (Test-Path $WorktreeDir) { Remove-Item -Recurse -Force $WorktreeDir }
}

function Invoke-NativeLoggedWithRetry {
    param($FilePath, $ArgumentList, $LogFile, $WorkingDirectory, $RunLogFile, $StepName, $MaxAttempts = 1, $RetryDelaySeconds = 2)
    $LastExitCode = 0
    for ($Attempt = 1; $Attempt -le $MaxAttempts; $Attempt++) {
        if ($Attempt -gt 1) { Write-RunLog -LogFile $RunLogFile -Message ("Retrying {0} ({1}/{2})" -f $StepName, $Attempt, $MaxAttempts) }
        $LastExitCode = Invoke-NativeLogged -FilePath $FilePath -ArgumentList $ArgumentList -LogFile $LogFile -WorkingDirectory $WorkingDirectory -RunLogFile $RunLogFile
        if ($LastExitCode -eq 0) { return 0 }
        if ($Attempt -lt $MaxAttempts) { Start-Sleep -Seconds $RetryDelaySeconds }
    }
    return $LastExitCode
}

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$IdfPath = "C:\Espressif\v5.5.4\esp-idf"
$PythonEnvRoot = "C:\Users\77905\.espressif\python_env\idf5.5_py3.11_env"
$PythonExe = "C:\Users\77905\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"

$IdfExportEnv = Join-Path (Split-Path $ProjectRoot -Parent) "idf-export.env"
if (Test-Path $IdfExportEnv) {
    Write-Host "Loading environment from $IdfExportEnv..."
    Get-Content $IdfExportEnv | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") {
            $Key = $matches[1].Trim()
            $Val = $matches[2].Trim()
            if ($Key -ne "PATH") { Set-Item -Path "Env:$Key" -Value $Val }
            else {
                $NewPathParts = $Val.Replace("%PATH%", "").Split(";") | Where-Object { $_ }
                $CurrentPathParts = $env:PATH.Split(";") | Where-Object { $_ }
                $env:PATH = (($NewPathParts + $CurrentPathParts) | Select-Object -Unique) -join ";"
            }
        }
    }
}

$ToolPaths = @()
$ToolPaths += "C:\Users\77905\.espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin"
$ToolPaths += "C:\Users\77905\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin"
$ToolPaths += "C:\Users\77905\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
$ToolPaths += "C:\Users\77905\.espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin"
$ToolPaths += "C:\Users\77905\.espressif\tools\cmake\3.30.2\bin"
$ToolPaths += "C:\Users\77905\.espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin"
$ToolPaths += "C:\Users\77905\.espressif\tools\ninja\1.12.1"
$ToolPaths += "C:\Users\77905\.espressif\tools\idf-exe\1.0.3"
$ToolPaths += "C:\Users\77905\.espressif\tools\dfu-util\0.11\dfu-util-0.11-win64"
$ToolPaths += "C:\Users\77905\.espressif\python_env\idf5.5_py3.11_env\Scripts"
$ToolPaths += "C:\Espressif\v5.5.4\esp-idf\tools"

$AllPaths = @($IdfPath, $PythonEnvRoot, $PythonExe) + $ToolPaths
foreach ($PathEntry in $AllPaths) { if (-not (Test-Path $PathEntry)) { throw "Required path not found: $PathEntry" } }

$LogRoot = Join-Path $ProjectRoot "logs"
$RunId = Get-Date -Format "yyyyMMdd-HHmmss"
$RunLogDir = Join-Path $LogRoot "build-native-$RunId"
New-Item -ItemType Directory -Path $RunLogDir -Force | Out-Null

$RunLog = Join-Path $RunLogDir "run.log"
$FetchReposLog = Join-Path $RunLogDir "fetch-repos.log"
$IdfSubmoduleLog = Join-Path $RunLogDir "idf-submodules.log"
$BuildLog = Join-Path $RunLogDir "build-native-run.log"
$LastRunPointer = Join-Path $ProjectRoot "build-native-last-run.txt"

Set-Content -Path $RunLog -Value "Build run id: $RunId", "Project root: $ProjectRoot", "Run log directory: $RunLogDir"
Set-Content -Path $LastRunPointer -Value $RunLogDir
Set-Content -Path (Join-Path $ProjectRoot "build-native-run.log") -Value "Current build run id: $RunId", "Current run log directory: $RunLogDir"

$env:IDF_PATH = $IdfPath
$env:ESP_IDF_VERSION = "5.5.4"
$env:IDF_CCACHE_ENABLE = "0"
$env:IDF_PYTHON_ENV_PATH = $PythonEnvRoot

# 设置本地组件管理器缓存路径，避免全局 AppData 权限问题
$LocalComponentCache = "E:\STACKCHAN\StackChan\firmware\.cache"
if (-not (Test-Path $LocalComponentCache)) { New-Item -ItemType Directory -Path $LocalComponentCache -Force | Out-Null }
Set-Item -Path "Env:IDF_COMPONENT_MANAGER_CACHE_PATH" -Value $LocalComponentCache
Set-Item -Path "Env:IDF_COMPONENT_MANAGER_STORAGE_URL" -Value "https://components.espressif.com/"
Set-Item -Path "Env:IDF_COMPONENT_MANAGER_ENABLED" -Value "1"
Set-Item -Path "Env:TERM" -Value "dumb"

$env:PATH = (($ToolPaths + $env:PATH.Split(";")) | Where-Object { $_ } | Select-Object -Unique) -join ";"

Push-Location $ProjectRoot
try {
    Write-RunLog -LogFile $RunLog -Message "Starting build-native.ps1 (skipping fetch_repos.py to preserve local fixes)"
    # $FetchExitCode = Invoke-NativeLogged -FilePath $PythonExe -ArgumentList @("$ProjectRoot\fetch_repos.py") -LogFile $FetchReposLog -WorkingDirectory $ProjectRoot -RunLogFile $RunLog
    # Publish-LatestLog -SourcePath $FetchReposLog -DestinationPath (Join-Path $ProjectRoot "fetch-repos.log")
    # if ($FetchExitCode -ne 0) { throw "fetch_repos.py failed with exit code $FetchExitCode. See $FetchReposLog" }

    $IdfSubmoduleStatusExitCode = Invoke-NativeLogged -FilePath "git" -ArgumentList @("-C", $IdfPath, "submodule", "status", "--recursive") -LogFile $IdfSubmoduleLog -WorkingDirectory $ProjectRoot -RunLogFile $RunLog
    Publish-LatestLog -SourcePath $IdfSubmoduleLog -DestinationPath (Join-Path $ProjectRoot "idf-submodules.log")

    $IdfSubmoduleLogContent = if (Test-Path $IdfSubmoduleLog) { Get-Content -Path $IdfSubmoduleLog -Raw } else { "" }
    $OutOfDateSubmodules = New-Object System.Collections.Generic.List[string]
    foreach ($Line in ($IdfSubmoduleLogContent -split "\r?\n")) {
        if ($Line -match '^\s*[\+\-U]\S+\s+([^\s]+)') { $OutOfDateSubmodules.Add($Matches[1]) }
    }

    if (($IdfSubmoduleStatusExitCode -eq 0) -and ($OutOfDateSubmodules.Count -eq 0)) {
        Write-RunLog -LogFile $RunLog -Message "ESP-IDF submodules are already present; skipping recursive update"
    } else {
        $IdfUpdateArguments = @("-C", $IdfPath, "submodule", "update", "--init", "--recursive", "--jobs", "1")
        if ($OutOfDateSubmodules.Count -gt 0) {
            Write-RunLog -LogFile $RunLog -Message ("Updating out-of-date ESP-IDF submodules: {0}" -f ($OutOfDateSubmodules -join ", "))
            $IdfUpdateArguments += $OutOfDateSubmodules
        }
        $IdfSubmoduleExitCode = Invoke-NativeLoggedWithRetry -FilePath "git" -ArgumentList $IdfUpdateArguments -LogFile $IdfSubmoduleLog -WorkingDirectory $ProjectRoot -RunLogFile $RunLog -StepName "ESP-IDF submodule update" -MaxAttempts 2
        if ($IdfSubmoduleExitCode -ne 0) {
            $LogContent = if (Test-Path $IdfSubmoduleLog) { Get-Content -Path $IdfSubmoduleLog -Raw } else { "" }
            if ($LogContent -match "components/esp_phy/lib") {
                Repair-GitSubmoduleState -RepoRoot $IdfPath -SubmodulePath "components/esp_phy/lib"
                Invoke-NativeLogged -FilePath "git" -ArgumentList @("-C", $IdfPath, "submodule", "update", "--init", "--recursive", "components/esp_phy/lib") -LogFile $IdfSubmoduleLog -WorkingDirectory $ProjectRoot -RunLogFile $RunLog
            }
        }
    }

    $BuildDir = Join-Path $ProjectRoot "build"
    if ((Test-Path $BuildDir) -and -not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) { Remove-Item -Recurse -Force $BuildDir }

    Write-RunLog -LogFile $RunLog -Message ("Running directly: {0} {1}" -f $PythonExe, "$IdfPath\tools\idf.py build")
    & $PythonExe "$IdfPath\tools\idf.py" "-DCCACHE_ENABLE=0" "build"
    $BuildExitCode = $LASTEXITCODE

    $IdfLogDir = Join-Path $BuildDir "log"
    $LatestStdOutLog = Get-ChildItem -Path $IdfLogDir -Filter "idf_py_stdout_output_*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $LatestStdErrLog = Get-ChildItem -Path $IdfLogDir -Filter "idf_py_stderr_output_*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $BuildLogPaths = @()
    if ($LatestStdOutLog) { $BuildLogPaths += $LatestStdOutLog.FullName }
    if ($LatestStdErrLog) { $BuildLogPaths += $LatestStdErrLog.FullName }
    Merge-LogFiles -DestinationPath $BuildLog -SourcePaths $BuildLogPaths
    Publish-LatestLog -SourcePath $BuildLog -DestinationPath (Join-Path $ProjectRoot "build-native-run.log")
    if ($BuildExitCode -ne 0) { throw "idf.py build failed with exit code $BuildExitCode. See $BuildLog" }
    Write-RunLog -LogFile $RunLog -Message "Build completed successfully"
} catch {
    if (Test-Path $RunLog) { Write-RunLog -LogFile $RunLog -Message ("Build failed: {0}" -f $_.Exception.Message) }
    throw
} finally {
    Pop-Location
}
