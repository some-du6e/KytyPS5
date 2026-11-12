<#
.SYNOPSIS
  Zips the newest KytyPS5 session log, _kyty.txt and a `vulkaninfo --summary` dump for a bug report.

.DESCRIPTION
  Run it from the KytyPS5 folder (the one with the emulator .exe), or pass -KytyDir.
  It reads:  <KytyDir>\_Logs\*.log (newest one), <KytyDir>\_kyty.txt (printf log, if enabled)
  and writes kyty-logs-<date>.zip in the current folder. Nothing is uploaded.

  A printf log larger than -MaxPrintfMB is cut to its first 32 MiB and last 128 MiB (the crash
  report is at the end); the full size is noted in collect_info.txt.

  The logs contain your Windows user name and the game folder path. Run
  `python tools\triage\kyty_triage.py --redact <zip>` to get a redacted summary to paste instead.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File collect_logs.ps1 -KytyDir "D:\KytyPS5"
#>
param(
    [string]$KytyDir = (Get-Location).Path,
    [string]$OutDir = (Get-Location).Path,
    [int]$MaxPrintfMB = 256
)

$ErrorActionPreference = "Stop"
$KytyDir = (Resolve-Path -LiteralPath $KytyDir).Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$zipPath = Join-Path $OutDir "kyty-logs-$stamp.zip"
$n = 1
while (Test-Path -LiteralPath $zipPath) { $zipPath = Join-Path $OutDir "kyty-logs-$stamp-$n.zip"; $n++ }
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "kyty-logs-$stamp"
New-Item -ItemType Directory -Path $stage | Out-Null
$info = New-Object System.Collections.Generic.List[string]
$info.Add("collected: $(Get-Date -Format o)")
$info.Add("kyty dir: $KytyDir")
$info.Add("os: $([System.Environment]::OSVersion.VersionString)")

# Copies a file even while the emulator still has it open for writing.
function Copy-Shared([string]$from, [string]$to, [long]$offset = 0, [long]$count = -1) {
    $in = [System.IO.File]::Open($from, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                 [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    try {
        $out = [System.IO.File]::Create($to)
        try {
            [void]$in.Seek($offset, [System.IO.SeekOrigin]::Begin)
            $buf = New-Object byte[] (1MB)
            $left = if ($count -lt 0) { [long]::MaxValue } else { $count }
            while ($left -gt 0) {
                $n = $in.Read($buf, 0, [int][Math]::Min($buf.Length, $left))
                if ($n -le 0) { break }
                $out.Write($buf, 0, $n)
                $left -= $n
            }
        } finally { $out.Dispose() }
    } finally { $in.Dispose() }
}

try {
    $logs = Join-Path $KytyDir "_Logs"
    $session = $null
    if (Test-Path -LiteralPath $logs) {
        $session = Get-ChildItem -LiteralPath $logs -Filter "*.log" -File |
                   Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }
    if ($session) {
        Copy-Shared $session.FullName (Join-Path $stage $session.Name)
        $info.Add("session log: $($session.Name) ($($session.Length) bytes, $($session.LastWriteTime))")
    } else {
        $info.Add("session log: none in $logs (start the game from the launcher, or pass --session-log)")
    }

    $printf = Join-Path $KytyDir "_kyty.txt"
    if (Test-Path -LiteralPath $printf) {
        $len = (Get-Item -LiteralPath $printf).Length
        $max = [long]$MaxPrintfMB * 1MB
        if ($len -le $max) {
            Copy-Shared $printf (Join-Path $stage "_kyty.txt")
            $info.Add("printf log: _kyty.txt ($len bytes)")
        } else {
            $head = 32MB
            $tail = 128MB
            Copy-Shared $printf (Join-Path $stage "_kyty.head.txt") 0 $head
            Copy-Shared $printf (Join-Path $stage "_kyty.tail.txt") ($len - $tail) $tail
            $info.Add("printf log: _kyty.txt is $len bytes; kept the first $head and the last $tail bytes")
        }
    } else {
        $info.Add("printf log: no _kyty.txt (it is off by default: --printf-direction File)")
    }

    $vk = Get-Command "vulkaninfo", "vulkaninfoSDK" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vk) {
        $vkOut = & $vk.Source --summary 2>&1 | Out-String
        Set-Content -LiteralPath (Join-Path $stage "vulkaninfo.txt") -Value $vkOut -Encoding UTF8
        $info.Add("vulkaninfo: $($vk.Source)")
    } else {
        $info.Add("vulkaninfo: not found (it comes with the GPU driver or the Vulkan SDK)")
    }

    Set-Content -LiteralPath (Join-Path $stage "collect_info.txt") -Value $info -Encoding UTF8
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "Wrote $zipPath"
    Write-Host "It contains your user name and game folder path. Attach it, or paste the output of:"
    Write-Host "  python tools\triage\kyty_triage.py --redact `"$zipPath`""
} finally {
    # $stage is the folder created above under the temp directory; nothing else is removed.
    if ($stage -and $stage.StartsWith([System.IO.Path]::GetTempPath()) -and (Test-Path -LiteralPath $stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
