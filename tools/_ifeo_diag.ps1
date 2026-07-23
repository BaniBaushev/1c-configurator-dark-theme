
$key = "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\1cv8.exe"
$exe = "$env:TEMP\ifeo_test\1cv8.exe"
$log = "D:\1CTheme\Настройка темы в конфигураторе 1С\builds\themehook_v3_log.txt"
$out = @()
function Test-Launch($label) {
    if (Test-Path $log) { Remove-Item $log -Force }
    $p = Start-Process $exe -PassThru
    Start-Sleep 4
    $alive = -not $p.HasExited
    if ($alive) { Stop-Process -Id $p.Id -Force }
    $logged = Test-Path $log
    return "$label => alive=$alive log=$logged"
}
# A: GlobalFlag only, no VerifierDlls
Set-ItemProperty -Path "Registry::$key" -Name GlobalFlag -Value 0x100
Remove-ItemProperty -Path "Registry::$key" -Name VerifierDlls -ErrorAction SilentlyContinue
$out += Test-Launch "A GlobalFlag-only"
# B: bare name, DLL in SysWOW64
Copy-Item "D:\1CTheme\Настройка темы в конфигураторе 1С\builds\ThemeLoader.dll" "C:\Windows\SysWOW64\ThemeLoader.dll" -Force
Set-ItemProperty -Path "Registry::$key" -Name VerifierDlls -Value "ThemeLoader.dll"
$out += Test-Launch "B bare-name SysWOW64"
# C: full path (known bad, reconfirm with new loader)
Set-ItemProperty -Path "Registry::$key" -Name VerifierDlls -Value "D:\1CTheme\Настройка темы в конфигураторе 1С\builds\ThemeLoader.dll"
$out += Test-Launch "C full-path"
$out | Out-File "$env:TEMP\ifeo_results.txt" -Encoding utf8
