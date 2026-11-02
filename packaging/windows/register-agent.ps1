# Registers nesva-agent as a per-user Windows scheduled task.
#
# Runs at user logon, with the user's own credentials and in the interactive
# session, because cloud storage and the Windows Credential Manager are
# per-user resources. Never register this task as SYSTEM.
#
# Usage (PowerShell, no elevation required):
#   powershell -ExecutionPolicy Bypass -File register-agent.ps1
#   powershell -ExecutionPolicy Bypass -File register-agent.ps1 -Executable "C:\Path\nesva-agent.exe"

param(
    [string]$Executable = "$env:LOCALAPPDATA\Programs\Nesva\nesva-agent.exe",
    [string]$TaskName = "NesvaAgent"
)

if (-not (Test-Path $Executable)) {
    Write-Error "nesva-agent.exe not found at $Executable. Pass -Executable with the correct path."
    exit 1
}

$action = New-ScheduledTaskAction -Execute $Executable
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Days 1)
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force `
    -Description "Nesva background backup agent (per-user)" | Out-Null

Write-Host "Registered scheduled task '$TaskName' for user $env:USERNAME."
Write-Host "Start now:  Start-ScheduledTask -TaskName $TaskName"
Write-Host "Status:     nesva-cli agent status"
Write-Host "Remove:     Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false"
