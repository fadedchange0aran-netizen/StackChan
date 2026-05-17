$ProjectRoot = "E:\STACKCHAN\StackChan\firmware"
$IdfPath = "C:\Espressif\v5.5.4\esp-idf"
$PythonExe = "C:\Users\77905\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"

$env:IDF_PATH = $IdfPath
$env:IDF_PYTHON_ENV_PATH = "C:\Users\77905\.espressif\python_env\idf5.5_py3.11_env"
$env:PATH = "C:\Users\77905\.espressif\tools\cmake\3.30.2\bin;C:\Users\77905\.espressif\tools\ninja\1.12.1;C:\Users\77905\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\v5.5.4\esp-idf\tools;" + $env:PATH

Push-Location $ProjectRoot
Write-Host "--- 正在编译并烧录程序 ---" -ForegroundColor Cyan
& $PythonExe "$IdfPath\tools\idf.py" build flash
Pop-Location
