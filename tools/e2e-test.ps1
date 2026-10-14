<#
    End-to-end check of the dictation loop without a human speaker:

      1. launches WinWhisper
      2. focuses Notepad (that is the "caret" the text must land in)
      3. presses the global hotkey
      4. synthesises a Russian phrase through the speakers, with a pause in the
         middle so the pause detector has something to cut on
      5. presses ESC and reads back what Notepad received

    Requires that the microphone can hear the speakers.

    Usage:
        pwsh -File tools\e2e-test.ps1
#>

param(
    [string]$Exe = 'D:\RocketScience\AI\win-whisper\x64\Release\WinWhisper.exe',
    [string]$Phrase1 = 'Привет, это проверка голосового ввода.',
    [string]$Phrase2 = 'Второе предложение для проверки.'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Speech

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class E2E {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
"@ -ErrorAction SilentlyContinue

function Tap-Key([byte]$vk) {
    [E2E]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 45
    [E2E]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
}

function Focus-Window([IntPtr]$handle) {
    $target = [E2E]::GetWindowThreadProcessId($handle, [IntPtr]::Zero)
    $current = [E2E]::GetCurrentThreadId()
    [void][E2E]::AttachThreadInput($current, $target, $true)
    [void][E2E]::SetForegroundWindow($handle)
    [void][E2E]::AttachThreadInput($current, $target, $false)
    Start-Sleep -Milliseconds 300
}

Get-Process WinWhisper -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700

$app = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 2
if ($app.HasExited) { throw 'WinWhisper exited immediately' }

$note = Start-Process notepad -PassThru
Start-Sleep -Seconds 1.5
$note.Refresh()
if ($note.MainWindowHandle -eq [IntPtr]::Zero) {
    # Some builds hand the window to a different process than the launcher.
    $note = Get-Process notepad -ErrorAction Stop |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}
if ($null -eq $note) { throw 'notepad window not found' }
Focus-Window $note.MainWindowHandle
Start-Sleep -Milliseconds 400

Write-Host '[e2e] starting dictation'
[E2E]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
[E2E]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero)
[E2E]::keybd_event(0x48, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[E2E]::keybd_event(0x48, 0, 2, [UIntPtr]::Zero)
[E2E]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero)
[E2E]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 800

# Notepad must stay focused: the app remembers it and types back into it.
Focus-Window $note.MainWindowHandle

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.Rate = 0
$synth.Volume = 100
$synth.SetOutputToDefaultAudioDevice()
Write-Host "[e2e] speaking: $Phrase1"
$synth.Speak($Phrase1)
Start-Sleep -Milliseconds 1600          # the pause the detector should cut on
Write-Host "[e2e] speaking: $Phrase2"
$synth.Speak($Phrase2)
Start-Sleep -Milliseconds 1800
$synth.Dispose()

Write-Host '[e2e] ESC'
Tap-Key 0x1B
Start-Sleep -Seconds 6                   # let the tail upload and get typed

Focus-Window $note.MainWindowHandle
[System.Windows.Forms.SendKeys]::SendWait('^a')
Start-Sleep -Milliseconds 200
[System.Windows.Forms.SendKeys]::SendWait('^c')
Start-Sleep -Milliseconds 400
$typed = [System.Windows.Forms.Clipboard]::GetText()

Write-Host ''
Write-Host '=== Notepad contents ==='
Write-Host $typed
Write-Host '========================'
if ([string]::IsNullOrWhiteSpace($typed)) {
    Write-Host '[e2e] RESULT: nothing was typed'
} else {
    Write-Host '[e2e] RESULT: text arrived'
}

Write-Host ''
Write-Host '=== WinWhisper log (tail) ==='
Get-Content (Join-Path $env:APPDATA 'WinWhisper\winwhisper.log') -Encoding Unicode -ErrorAction SilentlyContinue |
    Select-Object -Last 25

$note | Stop-Process -Force -ErrorAction SilentlyContinue
$app | Stop-Process -Force -ErrorAction SilentlyContinue