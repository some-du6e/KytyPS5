<#
.SYNOPSIS
  Runs the KytyPS5 GPU tests on this PC, or the import report and a first boot of a game, and zips
  the results to send back.

.DESCRIPTION
  Double-click preflight.cmd next to this file, or run it from a command prompt in this folder:

    preflight.cmd                          every GPU test in gpu-tests\tests.json, one at a time
    preflight.cmd -Quick                   only the tests tests.json marks quick
    preflight.cmd -Only a,b                only the tests named a and b
    preflight.cmd -Game "D:\PS5\GTA6"      the import report of the game, then a 300 s boot

  -Game <folder>       a decrypted game dump (the folder with eboot.bin)
  -BootSeconds <n>     how long the boot runs before preflight closes the emulator (default 300;
                       0: until you close the emulator yourself)
  -ExtraArgs "<opts>"  more emulator options for the import report and the boot, for example
                       -ExtraArgs "--title-profile gta6 --nid-names D:\PS5\names.txt". Give paths
                       in full: the emulator runs in the results folder
  -WithTests           with -Game, run the GPU tests too (before the boot; -Quick and -Only apply)
  -Emulator <path>     the kyty_emulator.exe to use (default: the one in the folder above this one)
  -OutDir <folder>     where the results go (default: this script's folder)

  Results: <OutDir>\kyty-preflight-<computer>-<yyyyMMdd-HHmmss>\ and a .zip of it next to it.
  Exit code: 0 no step failed, 1 a test or emulator step failed, 2 the script could not run. A
  boot that did not crash is RAN, not PASS: preflight cannot tell whether the game works.
  Nothing needs administrator rights. Nothing is written outside -OutDir except the temp folder.
#>

# There is no param() block: the arguments are parsed below. A PowerShell binding error (an unknown
# -Flag, a missing value) would end the script with exit code 1 before it starts, and here 2 means
# "could not run" and 1 "a test failed".

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$OnWindows = [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture
$Utf8 = New-Object System.Text.UTF8Encoding $false
$NewLine = [System.Environment]::NewLine
$ScriptDir = $PSScriptRoot
$StartedAt = Get-Date
$ConsoleText = New-Object System.Text.StringBuilder
$Steps = New-Object System.Collections.Generic.List[object]
$Problems = New-Object System.Collections.Generic.List[string]
$SystemData = [ordered]@{}
$Package = [ordered]@{}
$Mode = 'tests'
$Options = $null
$RunDir = $null
$Running = $null        # the child process being waited for; the finally block stops it
$RunningStep = $null
$Completed = $false
$Failure = $null
$UsageProblem = $false
$StopRequested = $false
$CtrlCAsInput = $false
$ExitCode = 2
$System32 = $null
if ($OnWindows) {
    $System32 = Join-Path $env:SystemRoot 'System32'
    # A 32-bit PowerShell sees SysWOW64 as System32; the 64-bit DLLs are behind Sysnative.
    if (-not [System.Environment]::Is64BitProcess -and (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'Sysnative'))) {
        $System32 = Join-Path $env:SystemRoot 'Sysnative'
    }
}
$ComputerName = $env:COMPUTERNAME
if (-not $ComputerName) { $ComputerName = [System.Environment]::MachineName }
$ComputerName = $ComputerName -replace '[^A-Za-z0-9_.-]', '_'

# Exit codes worth a word in the summary. 0x141 (321) is the emulator's own fatal exit.
$KnownExitCodes = @{
    '0x00000141' = 'Kyty fatal error, the log has the report'
    '0xC0000005' = 'access violation'
    '0xC000001D' = 'illegal instruction (the CPU lacks an instruction the build uses, such as AVX2)'
    '0xC0000028' = 'bad stack while unwinding an exception'
    '0xC000007B' = 'a DLL is for the wrong CPU type'
    '0xC00000FD' = 'stack overflow'
    '0xC0000135' = 'a DLL was not found'
    '0xC0000139' = 'a DLL lacks a function the program needs (the DLL is too old)'
    '0xC0000142' = 'a DLL failed to initialize'
    '0xC000013A' = 'stopped with Ctrl+C'
    '0xC0000409' = 'abort() or a fail-fast exception: a test check failed, or an uncaught C++ exception'
}

# --- Text and output ------------------------------------------------------------------------------

$ProfilePattern = $null
$profileDir = $env:USERPROFILE
if (-not $profileDir) { $profileDir = $env:HOME }
if ($profileDir -and $profileDir.Trim('\', '/').Length -gt 3) {
    # Either separator, and the doubled backslashes of JSON; not when a longer name goes on.
    $parts = $profileDir.TrimEnd('\', '/') -split '[\\/]'
    $pattern = (($parts | ForEach-Object { [regex]::Escape($_) }) -join '(?:\\\\|\\|/)') + '(?![A-Za-z0-9_.-])'
    $ProfilePattern = New-Object System.Text.RegularExpressions.Regex($pattern, 'IgnoreCase')
}

# The user profile path (it names the Windows account) as %USERPROFILE%.
function Hide([string]$Text) {
    if ([string]::IsNullOrEmpty($Text) -or $null -eq $ProfilePattern) { return $Text }
    return $ProfilePattern.Replace($Text, '%USERPROFILE%')
}

# Device and driver UUIDs (and LUIDs) out of vulkaninfo's text.
function Hide-Identifiers([string]$Text) {
    return [regex]::Replace($Text, '(?im)^(\s*\w*(?:UUID|LUID)\w*\s*[=:])[^\r\n]*', '$1 (removed by preflight)')
}

# Prints a line, and keeps it for console.txt.
function Say([string]$Text = '', [string]$Color = '', [switch]$NoNewline) {
    $Text = Hide $Text
    if ($Color) {
        Write-Host $Text -ForegroundColor $Color -NoNewline:$NoNewline
    } else {
        Write-Host $Text -NoNewline:$NoNewline
    }
    [void]$ConsoleText.Append($Text)
    if (-not $NoNewline) { [void]$ConsoleText.Append($NewLine) }
}

function Add-Problem([string]$Text) {
    $Problems.Add((Hide $Text))
    Say "  ! $Text" 'Yellow'
}

function Write-Text([string]$Path, [string]$Text, [switch]$Append) {
    $Text = Hide $Text
    if ($Append) {
        [System.IO.File]::AppendAllText($Path, $Text, $Utf8)
    } else {
        [System.IO.File]::WriteAllText($Path, $Text, $Utf8)
    }
}

function Format-Seconds([double]$Seconds) {
    return [string]::Format($Invariant, '{0:0.0}', $Seconds)
}

function Format-Time([datetime]$Time) {
    return $Time.ToString('yyyy-MM-ddTHH:mm:sszzz', $Invariant)
}

function Format-ExitCode($Code) {
    if ($null -eq $Code) { return $null }
    return '0x' + ([int]$Code).ToString('X8', $Invariant)
}

function Describe-ExitCode($Code) {
    $hex = Format-ExitCode $Code
    if ($null -eq $hex) { return 'no exit code' }
    $text = "exit code $hex ($Code)"
    if ($KnownExitCodes.ContainsKey($hex)) { $text += ": $($KnownExitCodes[$hex])" }
    return $text
}

# A property of a ConvertFrom-Json, registry or dictionary object, or $Default when it has none.
function Get-Field($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name) -and $null -ne $Object[$Name]) { return $Object[$Name] }
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function Get-FullPath([string]$Path) {
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

# The last $Bytes of a file as text, also while another process writes it.
function Read-Tail([string]$Path, [long]$Bytes) {
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $take = [int][Math]::Min($Bytes, $stream.Length)
        [void]$stream.Seek(-$take, 'End')
        $buffer = New-Object byte[] $take
        $read = $stream.Read($buffer, 0, $take)
    } finally { $stream.Dispose() }
    return $Utf8.GetString($buffer, 0, $read)
}

# The last lines a child printed: what follows "--- stdout ---" in its log, without the markers.
function Get-LastLines([string]$Path, [int]$Count = 3) {
    try {
        $all = @((Read-Tail $Path 16384) -split '\r?\n')
        $start = [Array]::LastIndexOf($all, '--- stdout ---') + 1
        if ($start -ge $all.Count) { return @() }
        $lines = @($all[$start..($all.Count - 1)] | Where-Object { $_.Trim() -and $_ -notmatch '^--- ' })
        return @($lines | Select-Object -Last $Count | ForEach-Object { if ($_.Length -gt 200) { $_.Substring(0, 200) + '...' } else { $_ } })
    } catch {
        return @()
    }
}

# Whether a test lost the GPU device: the harness's last words are then "... returned
# ErrorDeviceLost" (vulkan.hpp's name for VK_ERROR_DEVICE_LOST).
function Test-DeviceLost([string]$Path) {
    try { return (Read-Tail $Path 65536) -match 'ErrorDeviceLost|VK_ERROR_DEVICE_LOST' } catch { return $false }
}

# Ctrl+C arrives as a key (see the main block), so that stopping early goes through the usual
# catch and finally, with every cmdlet still working.
function Test-StopKey {
    if (-not $CtrlCAsInput) { return $false }
    try {
        while ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::C -and ($key.Modifiers -band [ConsoleModifiers]::Control)) { return $true }
        }
    } catch { }
    return $false
}

function Stop-OnKey {
    if (Test-StopKey) {
        $script:StopRequested = $true
        throw 'stopped with Ctrl+C'
    }
}

# Waits $Seconds, in short waits so that Ctrl+C gets through.
function Wait-Seconds([int]$Seconds) {
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        Stop-OnKey
        Start-Sleep -Milliseconds 250
    }
}

# --- Arguments ------------------------------------------------------------------------------------

function New-UsageError([string]$Message) {
    $script:UsageProblem = $true
    return $Message
}

function Show-Usage {
    Say 'Usage: preflight.cmd [-Quick | -Only <test,...>] [-OutDir <folder>]'
    Say '       preflight.cmd -Game <dump folder> [-BootSeconds <n>] [-ExtraArgs "<emulator options>"]'
    Say '                     [-WithTests [-Quick | -Only <test,...>]] [-Emulator <kyty_emulator.exe>] [-OutDir <folder>]'
    Say 'README.md next to this script explains each option.'
}

# Splits "--a b 'c d'" into its words; single or double quotes group.
function Split-Words([string]$Text) {
    $words = New-Object System.Collections.Generic.List[string]
    $word = New-Object System.Text.StringBuilder
    $quote = [char]0
    $any = $false
    foreach ($c in $Text.ToCharArray()) {
        if ($quote -ne [char]0) {
            if ($c -eq $quote) { $quote = [char]0 } else { [void]$word.Append($c) }
            continue
        }
        if ($c -eq [char]'"' -or $c -eq [char]"'") { $quote = $c; $any = $true; continue }
        if ([char]::IsWhiteSpace($c)) {
            if ($any) { $words.Add($word.ToString()); [void]$word.Clear(); $any = $false }
            continue
        }
        [void]$word.Append($c)
        $any = $true
    }
    if ($quote -ne [char]0) { throw (New-UsageError "a quote is not closed in: $Text") }
    if ($any) { $words.Add($word.ToString()) }
    return $words.ToArray()
}

# The script's arguments as typed. powershell.exe -File and pwsh -File cut one that starts with '-'
# and holds a colon in two at the colon, so -ExtraArgs "--nid-names D:\names.txt" would reach $args
# as "--nid-names D" and "\names.txt". When this script is the -File of its process, the arguments
# come from the process command line instead; run from a PowerShell prompt, from $args.
function Get-ScriptArguments([object[]]$Fallback) {
    try {
        $all = @([System.Environment]::GetCommandLineArgs())
        for ($i = 1; $i -lt $all.Count - 1; $i++) {
            if ($all[$i] -notmatch '^(-|/|--)(f|fi|fil|file)$') { continue }
            if ([System.IO.Path]::GetFileName($all[$i + 1]) -ine [System.IO.Path]::GetFileName($PSCommandPath)) { break }
            if ($i + 2 -ge $all.Count) { return ,@() }
            return ,@($all[($i + 2)..($all.Count - 1)])
        }
    } catch { }
    return ,@($Fallback)
}

function Read-Arguments([object[]]$List) {
    $kinds = [ordered]@{
        Game = 'value'; Emulator = 'value'; OutDir = 'value'; BootSeconds = 'value'
        Only = 'list'; ExtraArgs = 'list'; Quick = 'switch'; WithTests = 'switch'; Help = 'switch'
    }
    $names = @($kinds.Keys)
    $result = @{
        Game = $null; Emulator = $null; OutDir = $null; BootSeconds = $null
        Only = (New-Object System.Collections.Generic.List[string])
        ExtraArgs = (New-Object System.Collections.Generic.List[string])
        Quick = $false; WithTests = $false; Help = $false
    }
    $i = 0
    while ($i -lt $List.Count) {
        $item = $List[$i]
        $i++
        if ($item -is [string] -and @('-?', '/?', '-h', '--help') -contains $item) { $result.Help = $true; continue }
        if ($item -isnot [string] -or -not $item.StartsWith('-') -or $item.Length -lt 2) {
            throw (New-UsageError "unexpected argument: $item")
        }
        $text = $item.Substring(1)
        $inline = $null
        $colon = $text.IndexOf(':')
        if ($colon -ge 0) {
            $inline = $text.Substring($colon + 1)
            $text = $text.Substring(0, $colon)
        }
        $hits = @($names | Where-Object { $_ -eq $text })
        if ($hits.Count -eq 0) {
            $hits = @($names | Where-Object { $_.StartsWith($text, [System.StringComparison]::OrdinalIgnoreCase) })
        }
        if ($hits.Count -ne 1) {
            $message = "unknown option: $item"
            if ($hits.Count -gt 1) { $message = "$item could be -$($hits -join ' or -')" }
            if ($item.StartsWith('--')) {
                $message += ". Options for the emulator go in -ExtraArgs, for example -ExtraArgs `"$item ...`""
            }
            throw (New-UsageError $message)
        }
        $name = $hits[0]
        $kind = $kinds[$name]
        if ($kind -eq 'switch') {
            $result[$name] = ($null -eq $inline) -or ($inline -notmatch '^(false|0|\$false)$')
            continue
        }
        if ($null -ne $inline -and $inline -ne '') {
            $value = $inline
        } elseif ($i -lt $List.Count) {
            $value = $List[$i]
            $i++
        } else {
            throw (New-UsageError "-$name needs a value")
        }
        if ($kind -eq 'list') {
            foreach ($part in @($value)) {
                if ($name -eq 'Only') {
                    foreach ($test in ([string]$part -split '[,;\s]+')) { if ($test) { $result.Only.Add($test) } }
                } else {
                    foreach ($word in @(Split-Words ([string]$part))) { $result.ExtraArgs.Add($word) }
                }
            }
        } else {
            if ($value -isnot [string]) { throw (New-UsageError "-$name takes one value") }
            $result[$name] = $value
        }
    }
    return $result
}

# --- Windows helpers ------------------------------------------------------------------------------

# Compiled once by Add-Type (C# 5, for Windows PowerShell 5.1); $NativeReady says whether it worked.
$NativeReady = $false
$InputMode = -1         # the console input mode from before QuickEditOff; the finally block restores it
$NativeCode = @'
using System;
using System.Runtime.InteropServices;

namespace KytyPreflight
{
    public static class Native
    {
        [DllImport("kernel32.dll")]
        public static extern uint SetErrorMode(uint mode);

        [DllImport("kernel32.dll")]
        static extern uint SetThreadExecutionState(uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr GetStdHandle(int which);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetConsoleMode(IntPtr handle, out uint mode);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetConsoleMode(IntPtr handle, uint mode);

        // ES_CONTINUOUS | ES_SYSTEM_REQUIRED: no sleep while preflight runs unattended, until
        // KeepAwake(false) or the end of this process.
        public static void KeepAwake(bool on)
        {
            SetThreadExecutionState(on ? 0x80000001u : 0x80000000u);
        }

        // Turns QuickEdit off in this console window, where a click starts a selection that holds
        // up every write to the console, and so preflight and its time limits, until Esc. Returns
        // the mode for RestoreInputMode, or -1 when there is nothing to restore.
        public static long QuickEditOff()
        {
            IntPtr input = GetStdHandle(-10); // STD_INPUT_HANDLE
            uint mode;
            if (!GetConsoleMode(input, out mode) || (mode & 0x40u) == 0) return -1;
            // The console takes the QuickEdit bit (0x40) only with ENABLE_EXTENDED_FLAGS (0x80).
            if (!SetConsoleMode(input, (mode | 0x80u) & ~0x40u)) return -1;
            return mode;
        }

        public static void RestoreInputMode(long mode)
        {
            if (mode >= 0) SetConsoleMode(GetStdHandle(-10), (uint)mode | 0x80u);
        }

        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimitInformation
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimitInformation
        {
            public BasicLimitInformation BasicLimitInformation;
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateJobObjectW(IntPtr attributes, IntPtr name);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimitInformation info, int length);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, int length, IntPtr returnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool TerminateJobObject(IntPtr job, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);

        // A job holding `process` that ends every process in it when its last handle closes, so
        // also when preflight itself ends. IntPtr.Zero when that fails.
        public static IntPtr CreateJob(IntPtr process)
        {
            IntPtr job = CreateJobObjectW(IntPtr.Zero, IntPtr.Zero);
            if (job == IntPtr.Zero) return IntPtr.Zero;
            ExtendedLimitInformation info = new ExtendedLimitInformation();
            info.BasicLimitInformation.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if (!SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimitInformation))) ||
                !AssignProcessToJobObject(job, process))
            {
                CloseHandle(job);
                return IntPtr.Zero;
            }
            return job;
        }

        // The ids of the running processes in the job and in the jobs nested in it.
        public static int[] JobProcessIds(IntPtr job)
        {
            const int capacity = 1024;
            int size = 8 + IntPtr.Size * capacity;
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                // JOBOBJECT_BASIC_PROCESS_ID_LIST: two DWORD counts, then the ids as ULONG_PTRs.
                if (!QueryInformationJobObject(job, 3, buffer, size, IntPtr.Zero)) return new int[0];
                int count = Math.Min(Marshal.ReadInt32(buffer, 4), capacity);
                int[] ids = new int[count];
                for (int i = 0; i < count; i++) ids[i] = (int)Marshal.ReadIntPtr(buffer, 8 + IntPtr.Size * i).ToInt64();
                return ids;
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        public static bool InJob(IntPtr process, IntPtr job)
        {
            bool result;
            return IsProcessInJob(process, job, out result) && result;
        }

        public static bool TerminateJob(IntPtr job)
        {
            return TerminateJobObject(job, 1);
        }

        public static void CloseJob(IntPtr job)
        {
            CloseHandle(job);
        }
    }
}
'@

# Loads the helpers above. A failure leaves $NativeReady false: preflight then runs without them.
function Initialize-Native {
    if (-not $OnWindows) { return }
    try {
        if ($null -eq ('KytyPreflight.Native' -as [type])) { Add-Type -TypeDefinition $NativeCode -ErrorAction Stop }
        $script:NativeReady = $true
    } catch {
        Add-Problem "could not load preflight's Windows helpers ($($_.Exception.Message)): a crashing test may wait for its time limit, and closing this window may leave a test running"
    }
}

# The processes in a child's job, each with a handle open: while one is open, Windows does not give
# the process's id to another one.
function Get-JobProcesses($Job) {
    $list = New-Object System.Collections.Generic.List[object]
    foreach ($id in @([KytyPreflight.Native]::JobProcessIds($Job))) {
        try {
            $process = [System.Diagnostics.Process]::GetProcessById($id)
            if ([KytyPreflight.Native]::InJob($process.Handle, $Job)) { $list.Add($process) } else { $process.Dispose() }
        } catch { }
    }
    return $list.ToArray()
}

# --- Child processes ------------------------------------------------------------------------------

# One argument, quoted the way the Windows C runtime (and .NET on Linux) splits a command line.
function Format-Argument([string]$Argument) {
    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') { return $Argument }
    $quoted = New-Object System.Text.StringBuilder
    [void]$quoted.Append('"')
    $slashes = 0
    foreach ($c in $Argument.ToCharArray()) {
        if ($c -eq [char]'\') { $slashes++; continue }
        if ($c -eq [char]'"') { [void]$quoted.Append([char]'\', 2 * $slashes + 1) } else { [void]$quoted.Append([char]'\', $slashes) }
        [void]$quoted.Append($c)
        $slashes = 0
    }
    [void]$quoted.Append([char]'\', 2 * $slashes)
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

function Join-Arguments([string[]]$Arguments) {
    return ((@($Arguments) | Where-Object { $null -ne $_ } | ForEach-Object { Format-Argument $_ }) -join ' ')
}

function Format-Command([string]$File, [string[]]$Arguments) {
    return ((Format-Argument $File) + ' ' + (Join-Arguments $Arguments)).TrimEnd()
}

function New-StartInfo([string]$File, [string[]]$Arguments, [string]$WorkingDirectory) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = Join-Arguments $Arguments
    if ($WorkingDirectory) { $info.WorkingDirectory = $WorkingDirectory }
    $info.UseShellExecute = $false
    # No console of its own, and Ctrl+C in this window does not reach it: the finally block stops it.
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return $info
}

# Runs a short command and returns its exit code and output.
function Invoke-Capture([string]$File, [string[]]$Arguments, [int]$TimeoutSeconds = 60, [switch]$NoStop) {
    $started = Get-Date
    $process = [System.Diagnostics.Process]::Start((New-StartInfo $File $Arguments ''))
    try { $process.StandardInput.Close() } catch { }
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $exited = $true
    while (-not $process.WaitForExit(250)) {
        if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { $exited = $false; break }
    }
    if (-not $exited -and -not $NoStop) {
        [void](Stop-Child ([pscustomobject]@{ Process = $process; Id = $process.Id; Job = $null; Started = $started }))
    }
    $text = ''
    if ($out.Wait(5000)) { $text += $out.Result }
    if ($err.Wait(5000)) { $text += $err.Result }
    $code = $null
    if ($process.HasExited) { $code = $process.ExitCode }
    return [pscustomobject]@{ ExitCode = $code; Output = $text; TimedOut = -not $exited }
}

# Starts a program with stdout appended to $LogPath (after $Header) and stderr in a file next to it.
# Both pipes are copied to their files on thread-pool threads, so a chatty child cannot block.
function Start-Child([string]$File, [string[]]$Arguments, [string]$WorkingDirectory, $Environment,
                     [string]$LogPath, [string]$Header) {
    Write-Text $LogPath ($Header + "--- stdout ---$NewLine")
    $info = New-StartInfo $File $Arguments $WorkingDirectory
    if ($null -ne $Environment) {
        foreach ($key in @($Environment.Keys)) { $info.EnvironmentVariables[[string]$key] = [string]$Environment[$key] }
    }
    $errorPath = [System.IO.Path]::ChangeExtension($LogPath, '.stderr.log')
    $stdout = [System.IO.File]::Open($LogPath, 'Append', 'Write', 'ReadWrite')
    $stderr = [System.IO.File]::Open($errorPath, 'Create', 'Write', 'ReadWrite')
    try {
        $process = [System.Diagnostics.Process]::Start($info)
    } catch {
        $stdout.Dispose()
        $stderr.Dispose()
        Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
        throw
    }
    # The child and whatever it starts go into a job that Windows ends with preflight: closing this
    # window then stops a running test too, instead of leaving it on the GPU. Without one, Stop-Child
    # falls back to taskkill.
    $job = $null
    if ($NativeReady) {
        try {
            $handle = [KytyPreflight.Native]::CreateJob($process.Handle)
            if ($handle -ne [IntPtr]::Zero) { $job = $handle }
        } catch { }
    }
    $child = [pscustomobject]@{
        Process = $process
        Id = $process.Id
        Job = $job
        Started = Get-Date
        Clock = [System.Diagnostics.Stopwatch]::StartNew()
        Log = $LogPath
        ErrorLog = $errorPath
        Streams = @($stdout, $stderr)
        Copies = @($process.StandardOutput.BaseStream.CopyToAsync($stdout), $process.StandardError.BaseStream.CopyToAsync($stderr))
    }
    try { $process.StandardInput.Close() } catch { }
    $script:Running = $child
    return $child
}

# Waits up to $Seconds (0: no limit), in short waits so that Ctrl+C gets through. $OnTick runs every
# 30 s. True when the process exited, false when the time ran out.
function Wait-Child($Child, [double]$Seconds, [scriptblock]$OnTick) {
    $next = 30
    while (-not $Child.Process.WaitForExit(250)) {
        Stop-OnKey
        $elapsed = $Child.Clock.Elapsed.TotalSeconds
        if ($Seconds -gt 0 -and $elapsed -ge $Seconds) { return $false }
        if ($null -ne $OnTick -and $elapsed -ge $next) { $null = & $OnTick; $next += 30 }
    }
    return $true
}

# True when the process $Id runs and started after $Since (not a process that reused the id).
function Test-Ours([int]$Id, [datetime]$Since) {
    $process = Get-Process -Id $Id -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $false }
    try { return $process.StartTime -ge $Since.AddSeconds(-2) } catch { return $false }
}

# The ids of the processes in $Known and of their descendants, including those whose parent has
# exited (an emulator LoadExec starts its successor, then exits). Windows only.
function Get-TreeIds([int[]]$Known, [datetime]$Since) {
    $ids = New-Object System.Collections.Generic.List[int]
    foreach ($id in $Known) { if (-not $ids.Contains($id)) { $ids.Add($id) } }
    try {
        $all = @(Get-CimInstance -ClassName Win32_Process -Property ProcessId, ParentProcessId, CreationDate -ErrorAction Stop)
    } catch {
        return $ids.ToArray()
    }
    $limit = $Since.AddSeconds(-2)
    for ($i = 0; $i -lt $ids.Count; $i++) {
        foreach ($p in $all) {
            $id = [int]$p.ProcessId
            if ([int]$p.ParentProcessId -eq $ids[$i] -and $id -ne $ids[$i] -and -not $ids.Contains($id) -and
                ($null -eq $p.CreationDate -or $p.CreationDate -ge $limit)) {
                $ids.Add($id)
            }
        }
    }
    return $ids.ToArray()
}

# The ids among $Ids, in their order, of processes that run the emulator itself: $Emulator's file
# name, without the --pipeline-helper of its helper processes. Windows only.
function Select-Emulators([int[]]$Ids, [string]$Emulator) {
    if (@($Ids).Count -eq 0) { return @() }
    $name = [System.IO.Path]::GetFileName($Emulator)
    $filter = (@($Ids) | ForEach-Object { "ProcessId = $_" }) -join ' OR '
    try {
        $all = @(Get-CimInstance -ClassName Win32_Process -Filter $filter -Property ProcessId, Name, CommandLine -ErrorAction Stop)
    } catch {
        return @()
    }
    $match = @{}
    foreach ($p in $all) {
        if ([string]$p.Name -ieq $name -and [string]$p.CommandLine -notmatch '(^|\s)--pipeline-helper(\s|$)') { $match[[int]$p.ProcessId] = $true }
    }
    return @(@($Ids) | Where-Object { $match.ContainsKey($_) })
}

# Stops a child and everything it started: the processes in its job, or without one, taskkill /T /F
# (.NET Framework has no Kill(true)). With $GraceSeconds it first asks the windows to close, so that
# the emulator shuts down and writes out its logs. Returns 'closed' or 'killed'.
function Stop-Child($Child, [int]$GraceSeconds = 0, [int[]]$Known = @()) {
    if (-not $OnWindows) {
        # .NET Core: the process and its descendants.
        try { $Child.Process.Kill($true) } catch { }
        [void]$Child.Process.WaitForExit(10000)
        return 'killed'
    }
    if ($null -ne $Child.Job) {
        # Only processes in the job: an id taken over by another program is never touched.
        if ($GraceSeconds -gt 0) {
            $asked = $false
            $processes = @(Get-JobProcesses $Child.Job)
            foreach ($process in $processes) {
                try { if ($process.MainWindowHandle -ne [IntPtr]::Zero -and $process.CloseMainWindow()) { $asked = $true } } catch { }
                $process.Dispose()
            }
            if ($asked) {
                $clock = [System.Diagnostics.Stopwatch]::StartNew()
                while ($clock.Elapsed.TotalSeconds -lt $GraceSeconds) {
                    if (@([KytyPreflight.Native]::JobProcessIds($Child.Job)).Count -eq 0) {
                        [void]$Child.Process.WaitForExit(5000)
                        return 'closed'
                    }
                    Start-Sleep -Milliseconds 250
                }
            }
        }
        [void][KytyPreflight.Native]::TerminateJob($Child.Job)
        [void]$Child.Process.WaitForExit(10000)
        return 'killed'
    }
    $ids = @(Get-TreeIds (@($Child.Id) + @($Known)) $Child.Started)
    if ($GraceSeconds -gt 0) {
        $asked = $false
        foreach ($id in $ids) {
            if (-not (Test-Ours $id $Child.Started)) { continue }
            try {
                $process = [System.Diagnostics.Process]::GetProcessById($id)
                if ($process.MainWindowHandle -ne [IntPtr]::Zero -and $process.CloseMainWindow()) { $asked = $true }
            } catch { }
        }
        if ($asked) {
            $clock = [System.Diagnostics.Stopwatch]::StartNew()
            while ($clock.Elapsed.TotalSeconds -lt $GraceSeconds) {
                if (@($ids | Where-Object { Test-Ours $_ $Child.Started }).Count -eq 0) {
                    [void]$Child.Process.WaitForExit(5000)
                    return 'closed'
                }
                Start-Sleep -Milliseconds 250
            }
        }
    }
    $taskkill = Join-Path $System32 'taskkill.exe'
    foreach ($id in $ids) {
        $alive = $false
        if ($id -eq $Child.Id) { $alive = -not $Child.Process.HasExited } else { $alive = Test-Ours $id $Child.Started }
        if ($alive) {
            try { [void](Invoke-Capture $taskkill @('/PID', [string]$id, '/T', '/F') 30 -NoStop) } catch { }
        }
    }
    [void]$Child.Process.WaitForExit(10000)
    return 'killed'
}

# After the process exited or was stopped: the rest of its output, with stderr after stdout in the
# same log. Returns the exit code, or $null when the process still runs.
function Complete-Child($Child) {
    # Whatever the child left running ends with its job, and so lets go of the output pipes.
    if ($null -ne $Child.Job) {
        try { [KytyPreflight.Native]::CloseJob($Child.Job) } catch { }
        $Child.Job = $null
    }
    foreach ($copy in $Child.Copies) { try { [void]$copy.Wait(5000) } catch { } }
    foreach ($stream in $Child.Streams) { try { $stream.Dispose() } catch { } }
    $script:Running = $null
    try {
        if ((Get-Item -LiteralPath $Child.ErrorLog).Length -gt 0) {
            $log = [System.IO.File]::Open($Child.Log, 'Append', 'Write', 'ReadWrite')
            try {
                $marker = $Utf8.GetBytes("$NewLine--- stderr ---$NewLine")
                $log.Write($marker, 0, $marker.Length)
                $source = [System.IO.File]::Open($Child.ErrorLog, 'Open', 'Read', 'ReadWrite, Delete')
                try { $source.CopyTo($log) } finally { $source.Dispose() }
            } finally { $log.Dispose() }
        }
        Remove-Item -LiteralPath $Child.ErrorLog -Force
    } catch {
        Add-Problem "could not merge $($Child.ErrorLog) into $($Child.Log): $($_.Exception.Message)"
    }
    if ($Child.Process.HasExited) { return $Child.Process.ExitCode }
    return $null
}

function New-Step([string]$Kind, [string]$Name) {
    $step = [ordered]@{
        kind = $Kind; name = $Name; status = 'NOT RUN'; exit_code = $null; exit_code_hex = $null
        seconds = $null; detail = $null; log = $null
    }
    $Steps.Add($step)
    return $step
}

function Set-Exit($Step, $Code) {
    $Step.exit_code = $Code
    $Step.exit_code_hex = Format-ExitCode $Code
}

# Windows marks downloaded files (Mark-of-the-Web); this clears it on the files the script runs.
function Unblock-Files([string[]]$Paths) {
    if (-not $OnWindows) { return }
    foreach ($path in $Paths) {
        if ($path -and (Test-Path -LiteralPath $path -PathType Leaf)) {
            try { Unblock-File -LiteralPath $path -ErrorAction Stop } catch { }
        }
    }
}

# --- System information ---------------------------------------------------------------------------

function Get-FileVersion([string]$Path) {
    if (-not [System.IO.File]::Exists($Path)) { return $null }
    $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    return New-Object System.Version $version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart, $version.FilePrivatePart
}

# The lines of system.txt; $SystemData gets the main facts for summary.json. No serial numbers
# or UUIDs.
function Get-SystemInfo {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("collected: $(Format-Time (Get-Date))")
    $lines.Add("powershell: $($PSVersionTable.PSVersion) $($PSVersionTable.PSEdition)")
    $SystemData.powershell = "$($PSVersionTable.PSVersion) $($PSVersionTable.PSEdition)"
    if (-not $OnWindows) {
        $os = [System.Environment]::OSVersion.VersionString
        $lines.Add("os: $os")
        $lines.Add("logical processors: $([System.Environment]::ProcessorCount)")
        $SystemData.os = $os
    } else {
        try {
            $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
            $current = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
            $release = Get-Field $current 'DisplayVersion' (Get-Field $current 'ReleaseId' '')
            $version = $os.Version
            $ubr = Get-Field $current 'UBR'
            if ($null -ne $ubr) { $version = "$version.$ubr" }
            $text = "$($os.Caption) $version $release $($os.OSArchitecture)" -replace '\s+', ' '
            $lines.Add("os: $text (build $($os.BuildNumber))")
            $SystemData.os = $text
        } catch { $lines.Add("os: not available ($($_.Exception.Message))") }
        try {
            foreach ($cpu in @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop)) {
                $text = "$(([string]$cpu.Name).Trim()) ($($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) threads, $($cpu.MaxClockSpeed) MHz)"
                $lines.Add("cpu: $text")
                $SystemData.cpu = $text
            }
        } catch { $lines.Add("cpu: not available ($($_.Exception.Message))") }
        try {
            $computer = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop
            $text = [string]::Format($Invariant, '{0:0.0} GiB', $computer.TotalPhysicalMemory / 1GB)
            $lines.Add("ram: $text")
            $SystemData.ram = $text
        } catch { $lines.Add("ram: not available ($($_.Exception.Message))") }
        $gpus = New-Object System.Collections.Generic.List[object]
        try {
            foreach ($gpu in @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)) {
                $entry = [ordered]@{ name = [string]$gpu.Name; driver = [string]$gpu.DriverVersion; driver_date = $null; adapter_ram_mib = $null }
                if ($gpu.DriverDate) { $entry.driver_date = ([datetime]$gpu.DriverDate).ToString('yyyy-MM-dd', $Invariant) }
                if ($gpu.AdapterRAM) { $entry.adapter_ram_mib = [long]([long]$gpu.AdapterRAM / 1MB) }
                $gpus.Add($entry)
                $lines.Add("gpu: $($entry.name), driver $($entry.driver) ($($entry.driver_date)), AdapterRAM $($entry.adapter_ram_mib) MiB (WMI stops at 4 GiB), $($gpu.VideoModeDescription) at $($gpu.CurrentRefreshRate) Hz")
            }
        } catch { $lines.Add("gpu: not available ($($_.Exception.Message))") }
        # The display adapter class keys hold the real VRAM size, a 64-bit value.
        $class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
        foreach ($key in @(Get-ChildItem -LiteralPath $class -ErrorAction SilentlyContinue)) {
            if ($key.PSChildName -notmatch '^\d{4}$') { continue }
            try {
                $values = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction Stop
                $memory = Get-Field $values 'HardwareInformation.qwMemorySize'
                if ($null -eq $memory) { continue }
                $description = [string](Get-Field $values 'DriverDesc' '?')
                $mib = [long]([long]$memory / 1MB)
                $lines.Add("vram (registry): $($description): $mib MiB")
                foreach ($entry in $gpus) { if ($entry.name -eq $description) { $entry.vram_mib = $mib } }
            } catch { }
        }
        $SystemData.gpus = $gpus.ToArray()
        $smi = Get-Command 'nvidia-smi' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($smi) {
            try {
                $r = Invoke-Capture $smi.Path @('--query-gpu=name,driver_version,memory.total', '--format=csv') 30
                $lines.Add('nvidia-smi:')
                foreach ($line in ($r.Output -split '\r?\n')) { if ($line.Trim()) { $lines.Add("  $($line.Trim())") } }
                $SystemData.nvidia_smi = $r.Output.Trim()
            } catch { $lines.Add("nvidia-smi: failed ($($_.Exception.Message))") }
        } else {
            $lines.Add('nvidia-smi: not found')
        }
        $loader = Get-FileVersion (Join-Path $System32 'vulkan-1.dll')
        if ($loader) {
            $SystemData.vulkan_loader = $loader.ToString()
            $lines.Add("vulkan loader: vulkan-1.dll $loader")
        } else {
            $SystemData.vulkan_loader = $null
            $lines.Add('vulkan loader: no vulkan-1.dll in System32 (it comes with the GPU driver)')
        }
        $runtime = [ordered]@{}
        foreach ($dll in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll')) {
            $version = Get-FileVersion (Join-Path $System32 $dll)
            if ($version) { $runtime[$dll] = $version.ToString() } else { $runtime[$dll] = $null }
            $shown = 'missing'
            if ($version) { $shown = $version.ToString() }
            $lines.Add("$($dll): $shown")
        }
        $SystemData.msvc_runtime = $runtime
    }
    $vulkaninfo = Get-Command 'vulkaninfo', 'vulkaninfoSDK' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vulkaninfo) {
        try {
            $r = Invoke-Capture $vulkaninfo.Path @('--summary') 60
            $lines.Add("vulkaninfo --summary (exit code $($r.ExitCode)):")
            foreach ($line in ((Hide-Identifiers $r.Output) -split '\r?\n')) { $lines.Add("  $line") }
        } catch { $lines.Add("vulkaninfo: failed ($($_.Exception.Message))") }
    } else {
        $lines.Add('vulkaninfo: not on PATH (it comes with the Vulkan SDK)')
    }
    return $lines.ToArray()
}

# The DLLs each packaged executable imports that neither its folder, System32 nor PATH has.
function Find-MissingDlls($Manifest, [string]$PackageDir) {
    $missing = @{}
    if (-not $OnWindows) { return $missing }
    $executables = Get-Field $Manifest 'executables'
    if ($null -eq $executables) { return $missing }
    $folders = @($System32) + @(($env:PATH -split ';') | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim().Trim('"') })
    foreach ($property in $executables.PSObject.Properties) {
        $folder = Split-Path -Parent (Join-Path $PackageDir ($property.Name -replace '/', '\'))
        $lost = New-Object System.Collections.Generic.List[string]
        foreach ($dll in @(Get-Field $property.Value 'system_dlls' @())) {
            if ($dll -match '^(api|ext)-ms-') { continue }  # API sets, part of Windows
            $found = $false
            foreach ($dir in @($folder) + $folders) {
                try { if ([System.IO.File]::Exists([System.IO.Path]::Combine($dir, $dll))) { $found = $true; break } } catch { }
            }
            if (-not $found) { $lost.Add($dll) }
        }
        if ($lost.Count -gt 0) { $missing[$property.Name] = $lost.ToArray() }
    }
    return $missing
}

# The tests and the emulator use the VC++ runtime DLLs. Builds of Visual Studio 2022 17.10 and
# newer (this one) can crash in std::mutex with an msvcp140.dll older than 14.40.
function Test-MsvcRuntime([string[]]$MissingDlls) {
    if (-not $OnWindows) { return }
    $redist = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
    $runtime = @(@($MissingDlls) | Where-Object { $_ -match '^(vcruntime|msvcp|concrt|vccorlib)\d' })
    if ($runtime.Count -gt 0) {
        Add-Problem "missing $($runtime -join ', '): install the Microsoft Visual C++ Redistributable (x64) from $redist, then run this again"
        return
    }
    $version = Get-FileVersion (Join-Path $System32 'msvcp140.dll')
    if ($null -ne $version -and $version -lt (New-Object System.Version 14, 40)) {
        Add-Problem "msvcp140.dll is $version; this build needs 14.40 or newer. Install the Microsoft Visual C++ Redistributable (x64) from $redist"
    }
}

# --- Steps ----------------------------------------------------------------------------------------

function Get-PackagePath([string]$PackageDir, [string]$Relative) {
    return Join-Path $PackageDir ($Relative -replace '/', [System.IO.Path]::DirectorySeparatorChar)
}

function Invoke-Tests($Manifest, [object[]]$Selected, [string]$PackageDir) {
    $testsDir = Join-Path $RunDir 'tests'
    New-Item -ItemType Directory -Force -Path $testsDir | Out-Null
    $missing = Find-MissingDlls $Manifest $PackageDir
    Test-MsvcRuntime @($missing.Values | ForEach-Object { $_ } | Sort-Object -Unique)
    $records = New-Object System.Collections.Generic.List[object]
    $toUnblock = New-Object System.Collections.Generic.List[string]
    $executables = Get-Field $Manifest 'executables'
    foreach ($test in $Selected) {
        $step = New-Step 'test' ([string]$test.name)
        $step.quick = [bool](Get-Field $test 'quick' $false)
        $step.timeout_seconds = [int](Get-Field $test 'timeout' 300)
        $records.Add($step)
        $exe = Get-PackagePath $PackageDir ([string]$test.executable)
        $toUnblock.Add($exe)
        foreach ($dll in @(Get-Field (Get-Field $executables ([string]$test.executable)) 'dlls' @())) {
            $toUnblock.Add((Join-Path (Split-Path -Parent $exe) $dll))
        }
    }
    Unblock-Files @($toUnblock | Sort-Object -Unique)
    $count = $Selected.Count
    $plural = ''
    if ($count -ne 1) { $plural = 's' }
    Say "Running $count GPU test$plural, one at a time. Close games and other programs that use the GPU."
    $colors = @{ PASS = 'Green'; FAIL = 'Red'; TIMEOUT = 'Yellow'; ERROR = 'Red' }
    for ($i = 0; $i -lt $count; $i++) {
        Save-Progress
        $test = $Selected[$i]
        $step = $records[$i]
        $name = [string]$test.name
        $relative = [string]$test.executable
        $exe = Get-PackagePath $PackageDir $relative
        $arguments = @(@(Get-Field $test 'args' @()) | ForEach-Object { [string]$_ })
        $timeout = $step.timeout_seconds
        $willFail = [bool](Get-Field $test 'will_fail' $false)
        $cwd = $testsDir
        $cwdRelative = Get-Field $test 'cwd'
        if ($cwdRelative) {
            $cwd = Get-PackagePath $PackageDir ([string]$cwdRelative)
            New-Item -ItemType Directory -Force -Path $cwd | Out-Null
        }
        $environment = $null
        $envObject = Get-Field $test 'env'
        if ($null -ne $envObject) {
            $environment = @{}
            foreach ($property in $envObject.PSObject.Properties) { $environment[$property.Name] = [string]$property.Value }
        }
        $file = $name -replace '[^A-Za-z0-9_.-]', '_'
        $log = Join-Path $testsDir "$file.log"
        $step.log = "tests/$file.log"
        Say "[$($i + 1)/$count] $name ... " -NoNewline
        if ($missing.ContainsKey($relative)) {
            $step.status = 'ERROR'
            $step.detail = "not run: $relative needs $($missing[$relative] -join ', '), which this PC does not have"
            Write-Text $log "test: $name$NewLine$($step.detail)$NewLine"
            Say 'ERROR (missing DLL)' 'Red'
            continue
        }
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            $step.status = 'ERROR'
            $step.detail = "not run: $exe is missing"
            Write-Text $log "test: $name$NewLine$($step.detail)$NewLine"
            Say 'ERROR (missing executable)' 'Red'
            continue
        }
        $header = "test: $name$NewLine" +
                  "command: $(Format-Command $exe $arguments)$NewLine" +
                  "folder: $cwd$NewLine"
        if ($null -ne $environment) {
            $header += "environment: $((@($environment.Keys) | Sort-Object | ForEach-Object { "$_=$($environment[$_])" }) -join ' ')$NewLine"
        }
        $expect = ''
        if ($willFail) { $expect = ', expected to fail (WILL_FAIL)' }
        $header += "timeout: $timeout s$expect$NewLine" + "started: $(Format-Time (Get-Date))$NewLine"
        $step.status = 'ERROR'
        $script:RunningStep = $step
        try {
            $child = Start-Child $exe $arguments $cwd $environment $log $header
        } catch {
            $script:RunningStep = $null
            $step.detail = "could not start: $($_.Exception.Message)"
            Write-Text $log "$($step.detail)$NewLine" -Append
            Say "ERROR ($($step.detail))" 'Red'
            continue
        }
        $dotState = @{ printed = $false }
        $exited = Wait-Child $child $timeout { Say '.' -NoNewline; $dotState.printed = $true }
        $seconds = $child.Clock.Elapsed.TotalSeconds
        if (-not $exited) { [void](Stop-Child $child) }
        $code = Complete-Child $child
        $script:RunningStep = $null
        Set-Exit $step $code
        $step.seconds = [Math]::Round($seconds, 1)
        if (-not $exited) {
            $step.status = 'TIMEOUT'
            $step.detail = "still running after $timeout s; preflight stopped it"
        } elseif ($null -eq $code) {
            $step.status = 'ERROR'
            $step.detail = 'no exit code'
        } elseif (($code -eq 0) -xor $willFail) {
            $step.status = 'PASS'
        } else {
            $step.status = 'FAIL'
            $step.detail = Describe-ExitCode $code
        }
        Write-Text $log "$NewLine--- $($name): $($step.status), $(Describe-ExitCode $code), $(Format-Seconds $seconds) s ---$NewLine" -Append
        $space = ''
        if ($dotState.printed) { $space = ' ' }
        $extra = ''
        if ($step.status -eq 'FAIL') { $extra = ", exit $($step.exit_code_hex)" }
        Say "$space$($step.status) ($(Format-Seconds $seconds) s$extra)" $colors[$step.status]
        if ($step.status -ne 'PASS') {
            foreach ($line in @(Get-LastLines $log 1)) { Say "      $line" 'DarkGray' }
        }
        # A lost device probably means Windows reset the GPU (a TDR). After 5 resets within 60 s it
        # stops the PC with a blue screen, so after one the next test waits a minute.
        if ($step.status -ne 'PASS' -and (Test-DeviceLost $log)) {
            $step.device_lost = $true
            if ($i -lt $count - 1) {
                Say '      The GPU was reset (ErrorDeviceLost). The next test starts in 60 s, so that Windows does not restart the PC.' 'Yellow'
                Wait-Seconds 60
            }
        }
    }
    Save-Progress
}

# Runs the emulator for one step, with its output in $LogName. Returns what happened ($null when it
# could not start); the caller judges it.
function Invoke-Emulator($Step, [string]$Emulator, [string[]]$Arguments, [string]$WorkingDirectory,
                         [string]$LogName, [double]$Seconds, [int]$GraceSeconds, [switch]$FollowRestarts) {
    $log = Join-Path $RunDir $LogName
    $Step.log = $LogName
    $limit = 'none'
    if ($Seconds -gt 0) { $limit = "$Seconds s" }
    $header = "step: $($Step.name)$NewLine" +
              "command: $(Format-Command $Emulator $Arguments)$NewLine" +
              "folder: $WorkingDirectory$NewLine" +
              "time limit: $limit$NewLine" +
              "started: $(Format-Time (Get-Date))$NewLine"
    $Step.status = 'ERROR'
    $script:RunningStep = $Step
    try {
        $child = Start-Child $Emulator $Arguments $WorkingDirectory $null $log $header
    } catch {
        $script:RunningStep = $null
        $Step.detail = "could not start the emulator: $($_.Exception.Message)"
        Write-Text $log "$($Step.detail)$NewLine" -Append
        return $null
    }
    $dotState = @{ printed = $false }
    $known = New-Object System.Collections.Generic.List[int]
    # The processes followed below, kept open until the step ends: their ids cannot go to another
    # process meanwhile.
    $held = New-Object System.Collections.Generic.List[object]
    $exited = Wait-Child $child $Seconds { Say '.' -NoNewline; $dotState.printed = $true }
    $code = $null
    $restarted = 0
    # An emulator LoadExec starts its successor and exits: the game goes on in that process, another
    # kyty_emulator.exe. Its pipeline helpers, its conhost.exe or a WerFault.exe do not count: the
    # exit code that judges the boot is the last emulator's.
    while ($exited -and $FollowRestarts -and $OnWindows) {
        if ($null -ne $child.Job) {
            $candidates = @([KytyPreflight.Native]::JobProcessIds($child.Job))
        } else {
            $candidates = @(Get-TreeIds (@($child.Id) + $known.ToArray()) $child.Started | Where-Object { Test-Ours $_ $child.Started })
        }
        $next = @(Select-Emulators @($candidates | Where-Object { $_ -ne $child.Id -and -not $known.Contains($_) }) $Emulator)
        if ($next.Count -eq 0) { break }
        $followed = @()
        foreach ($id in $next) {
            $known.Add($id)
            try {
                $p = [System.Diagnostics.Process]::GetProcessById($id)
                $null = $p.Handle
                $held.Add($p)
                if ($null -eq $child.Job -or [KytyPreflight.Native]::InJob($p.Handle, $child.Job)) { $followed += $p }
            } catch { }
        }
        if ($followed.Count -eq 0) { continue }
        $restarted++
        Say "(the emulator restarted itself as process $(@($followed | ForEach-Object { $_.Id }) -join ', ')) " -NoNewline
        foreach ($p in $followed) {
            while (-not $p.WaitForExit(250)) {
                Stop-OnKey
                if ($Seconds -gt 0 -and $child.Clock.Elapsed.TotalSeconds -ge $Seconds) { $exited = $false; break }
            }
            if (-not $exited) { break }
            $code = $p.ExitCode
        }
    }
    $elapsed = $child.Clock.Elapsed.TotalSeconds
    $stopped = $null
    if (-not $exited) { $stopped = Stop-Child $child $GraceSeconds $known.ToArray() }
    $rootCode = Complete-Child $child
    foreach ($p in $held) { try { $p.Dispose() } catch { } }
    $script:RunningStep = $null
    if ($restarted -eq 0) { $code = $rootCode }
    Set-Exit $Step $code
    $Step.seconds = [Math]::Round($elapsed, 1)
    if ($restarted -gt 0) { $Step.restarts = $restarted }
    $how = 'exited'
    if (-not $exited) { $how = "stopped by preflight ($stopped)" }
    Write-Text $log "$NewLine--- $($Step.name): $how, $(Describe-ExitCode $code), $(Format-Seconds $elapsed) s ---$NewLine" -Append
    if ($dotState.printed) { Say ' ' -NoNewline }
    return [pscustomobject]@{ Exited = $exited; Code = $code; Seconds = $elapsed; Stopped = $stopped }
}

function Invoke-Game([string]$Emulator, [string]$Game, [string[]]$ExtraArgs, [int]$BootSeconds,
                     [string]$Collect, $Manifest, [object[]]$Tests, [string]$PackageDir) {
    # The emulator's working folder: its _SaveData, _TempData and _kyty.txt go here, not next to
    # the emulator. The zip leaves it out (collect_logs.ps1 takes _kyty.txt from it).
    $cwd = Join-Path $RunDir 'emulator'
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null
    $emulatorDir = Split-Path -Parent $Emulator
    Unblock-Files (@($Emulator, $Collect) + @(Get-ChildItem -LiteralPath $emulatorDir -Filter '*.dll' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }))
    if ($null -ne $Manifest -and @($Tests).Count -eq 0) {
        # The tests are built like the emulator, so they import the same runtime DLLs.
        $missing = Find-MissingDlls $Manifest $PackageDir
        Test-MsvcRuntime @($missing.Values | ForEach-Object { $_ } | Sort-Object -Unique)
    }

    $imports = New-Step 'imports' 'imports'
    $importsFile = Join-Path $RunDir 'imports.txt'
    Say 'Import report (no GPU, no game code) ... ' -NoNewline
    $r = Invoke-Emulator $imports $Emulator (@('--game', $Game, '--dump-imports', $importsFile) + $ExtraArgs) $cwd 'emulator-imports.log' 600 0
    if ($null -eq $r) {
        Say "ERROR ($($imports.detail))" 'Red'
    } elseif (-not $r.Exited) {
        $imports.status = 'TIMEOUT'
        $imports.detail = 'still running after 600 s; preflight stopped it'
        Say "TIMEOUT ($(Format-Seconds $r.Seconds) s)" 'Yellow'
    } elseif ($r.Code -eq 0 -and (Test-Path -LiteralPath $importsFile -PathType Leaf)) {
        $imports.status = 'PASS'
        $imports.detail = "imports.txt, $((Get-Item -LiteralPath $importsFile).Length) bytes"
        Say "PASS ($(Format-Seconds $r.Seconds) s)" 'Green'
    } else {
        $imports.status = 'FAIL'
        $imports.detail = Describe-ExitCode $r.Code
        if ($r.Code -eq 0) { $imports.detail = 'exited with 0 but wrote no imports.txt' }
        Say "FAIL ($(Format-Seconds $r.Seconds) s, $($imports.detail))" 'Red'
        foreach ($line in @(Get-LastLines (Join-Path $RunDir 'emulator-imports.log') 2)) { Say "      $line" 'DarkGray' }
    }
    Save-Progress

    if (@($Tests).Count -gt 0) { Invoke-Tests $Manifest $Tests $PackageDir }

    $boot = New-Step 'boot' 'boot'
    $boot.boot_seconds = $BootSeconds
    $sessionLog = Join-Path $RunDir 'session.log'
    if ($BootSeconds -gt 0) {
        Say "Booting the game for $BootSeconds s. Leave this window open; close the game window to stop early."
    } else {
        Say 'Booting the game. Play as long as you like, then close the game window (not this one).'
    }
    Say 'Boot ... ' -NoNewline
    $arguments = @('--game', $Game, '--preset', 'rtx3050', '--session-log', $sessionLog) + $ExtraArgs
    $r = Invoke-Emulator $boot $Emulator $arguments $cwd 'emulator-boot.log' $BootSeconds 15 -FollowRestarts
    # A boot that did not crash is RAN, not PASS: a game frozen on a black screen runs too, and
    # closing the game window ends the emulator with 0. Only session.log tells how far it got.
    if ($null -eq $r) {
        Say "ERROR ($($boot.detail))" 'Red'
    } elseif (-not $r.Exited) {
        $how = 'stopped it'
        if ($r.Stopped -eq 'closed') { $how = 'closed its window' }
        $boot.status = 'RAN'
        $boot.detail = "still running after $BootSeconds s; preflight $how. session.log shows how far the game got"
        Say "RAN ($($boot.detail))" 'Cyan'
    } elseif ($r.Code -eq 0) {
        $boot.status = 'RAN'
        $boot.detail = "the emulator exited with 0 after $(Format-Seconds $r.Seconds) s. session.log shows how far the game got"
        Say "RAN ($($boot.detail))" 'Cyan'
    } else {
        $boot.status = 'FAIL'
        $boot.detail = "the emulator exited after $(Format-Seconds $r.Seconds) s, $(Describe-ExitCode $r.Code)"
        Say "FAIL ($($boot.detail))" 'Red'
        foreach ($line in @(Get-LastLines (Join-Path $RunDir 'emulator-boot.log') 2)) { Say "      $line" 'DarkGray' }
    }
    Save-Progress

    $logs = New-Step 'collect' 'logs'
    Say 'Collecting the logs ... ' -NoNewline
    if (-not (Test-Path -LiteralPath $Collect -PathType Leaf)) {
        $logs.status = 'FAIL'
        $logs.detail = 'collect_logs.ps1 is not next to preflight.ps1'
        Say "FAIL ($($logs.detail))" 'Red'
        return
    }
    try {
        # collect_logs.ps1 zips the newest session log of <KytyDir>\_Logs (there is none: this boot
        # wrote session.log), the printf log _kyty.txt of the emulator's folder (only with
        # --printf-direction File) and vulkaninfo. It was written without strict mode.
        $output = & {
            Set-StrictMode -Off
            & $Collect -KytyDir $cwd -OutDir $RunDir *>&1 | ForEach-Object { "$_" }
        }
        Write-Text (Join-Path $RunDir 'collect_logs.txt') ((@($output) -join $NewLine) + $NewLine)
        $zip = Get-ChildItem -LiteralPath $RunDir -Filter 'kyty-logs-*.zip' -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($null -eq $zip) { throw 'collect_logs.ps1 wrote no kyty-logs-*.zip' }
        Remove-ZipIdentifiers $zip.FullName
        $logs.status = 'PASS'
        $logs.detail = $zip.Name
        $logs.log = 'collect_logs.txt'
        Say "PASS ($($zip.Name))" 'Green'
    } catch {
        if ($StopRequested) { throw }
        $logs.status = 'FAIL'
        $logs.detail = "collect_logs.ps1 failed: $($_.Exception.Message)"
        Say "FAIL ($($logs.detail))" 'Red'
    }
}

# --- Zips -----------------------------------------------------------------------------------------

$ZipReady = $false
function Initialize-Zip {
    if ($script:ZipReady) { return }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $script:ZipReady = $true
}

# vulkaninfo.txt in collect_logs.ps1's zip, without device UUIDs and the profile path.
function Remove-ZipIdentifiers([string]$ZipPath) {
    Initialize-Zip
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($entry in @($zip.Entries | Where-Object { $_.Name -eq 'vulkaninfo.txt' })) {
            $reader = New-Object System.IO.StreamReader($entry.Open())
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
            $clean = Hide (Hide-Identifiers $text)
            if ($clean -ceq $text) { continue }
            $name = $entry.FullName
            $entry.Delete()
            $writer = New-Object System.IO.StreamWriter($zip.CreateEntry($name).Open(), $Utf8)
            try { $writer.Write($clean) } finally { $writer.Dispose() }
        }
    } finally { $zip.Dispose() }
}

# Zips $Folder into $ZipPath under the folder's name, with forward slashes, leaving out the
# top-level folders named in $Skip. Reads files that another process still has open.
function Write-Zip([string]$Folder, [string]$ZipPath, [string[]]$Skip) {
    Initialize-Zip
    $base = Split-Path -Leaf $Folder
    $files = New-Object System.Collections.Generic.List[object]
    foreach ($item in @(Get-ChildItem -LiteralPath $Folder -Force)) {
        if ($item.PSIsContainer) {
            if ($Skip -contains $item.Name) { continue }
            foreach ($file in @(Get-ChildItem -LiteralPath $item.FullName -Recurse -File -Force)) { $files.Add($file) }
        } else {
            $files.Add($item)
        }
    }
    $stream = [System.IO.File]::Open($ZipPath, 'CreateNew', 'ReadWrite', 'None')
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($file in $files) {
                $relative = $file.FullName.Substring($Folder.Length).TrimStart('\', '/') -replace '\\', '/'
                try {
                    $source = [System.IO.File]::Open($file.FullName, 'Open', 'Read', 'ReadWrite, Delete')
                } catch {
                    $Problems.Add((Hide "left out of the zip, could not read $($relative): $($_.Exception.Message)"))
                    continue
                }
                try {
                    $entry = $zip.CreateEntry("$base/$relative", [System.IO.Compression.CompressionLevel]::Optimal)
                    try { $entry.LastWriteTime = New-Object System.DateTimeOffset($file.LastWriteTime) } catch { }
                    $target = $entry.Open()
                    try { $source.CopyTo($target) } finally { $target.Dispose() }
                } finally { $source.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
}

# --- Summary --------------------------------------------------------------------------------------

# $Result RUNNING: a summary written along the way (Save-Progress), with no exit code yet.
function Write-Summary([string]$Result) {
    $code = $ExitCode
    if ($Result -eq 'RUNNING') { $code = $null }
    $counts = [ordered]@{}
    foreach ($status in @('PASS', 'RAN', 'FAIL', 'TIMEOUT', 'ERROR', 'INTERRUPTED', 'NOT RUN')) {
        $n = @($Steps | Where-Object { $_.status -eq $status }).Count
        if ($n -gt 0) { $counts[$status] = $n }
    }
    $finished = Get-Date
    $optionData = [ordered]@{}
    if ($null -ne $Options) {
        $optionData.game = $Options.Game
        $optionData.boot_seconds = $Options.BootSeconds
        $optionData.extra_args = @($Options.ExtraArgs)
        $optionData.quick = $Options.Quick
        $optionData.only = @($Options.Only)
        $optionData.with_tests = $Options.WithTests
        $optionData.emulator = $Options.Emulator
        $optionData.out_dir = $Options.OutDir
    }
    $summary = [ordered]@{
        format = 1
        result = $Result
        exit_code = $code
        mode = $Mode
        computer = $ComputerName
        started = Format-Time $StartedAt
        finished = Format-Time $finished
        seconds = [Math]::Round(($finished - $StartedAt).TotalSeconds, 1)
        options = $optionData
        package = $Package
        system = $SystemData
        counts = $counts
        steps = $Steps.ToArray()
        problems = $Problems.ToArray()
    }
    Write-Text (Join-Path $RunDir 'summary.json') ((ConvertTo-Json -InputObject $summary -Depth 8) + $NewLine)

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("KytyPS5 preflight on $ComputerName, $(Format-Time $StartedAt), $(Format-Seconds ($finished - $StartedAt).TotalSeconds) s")
    if ($null -eq $code) {
        $lines.Add("result: $Result (preflight had not finished when it wrote this)")
    } else {
        $lines.Add("result: $Result (exit code $code)")
    }
    $source = @()
    foreach ($key in @('repository', 'ref', 'commit', 'run_id')) { if ($Package.Contains($key)) { $source += "$key $($Package[$key])" } }
    if ($source.Count -gt 0) { $lines.Add("package: $($source -join ', ')") }
    foreach ($key in @('os', 'cpu', 'ram')) { if ($SystemData.Contains($key)) { $lines.Add("$($key): $($SystemData[$key])") } }
    if ($SystemData.Contains('gpus')) {
        foreach ($gpu in @($SystemData.gpus)) {
            $vram = ''
            if ($gpu.Contains('vram_mib')) { $vram = ", $($gpu.vram_mib) MiB" }
            $lines.Add("gpu: $($gpu.name), driver $($gpu.driver)$vram")
        }
    }
    if ($counts.Count -gt 0) { $lines.Add("steps: $((@($counts.Keys) | ForEach-Object { "$($counts[$_]) $_" }) -join ', ')") }
    $lines.Add('')
    foreach ($step in $Steps) {
        $time = ''
        if ($null -ne $step.seconds) { $time = "$(Format-Seconds $step.seconds) s" }
        $line = [string]::Format($Invariant, '{0,-11} {1,9}  {2,-8} {3}', $step.status, $time, $step.kind, $step.name)
        if ($step.detail) { $line += ": $($step.detail)" }
        $lines.Add($line)
        if ($step.status -ne 'PASS' -and $step.status -ne 'NOT RUN' -and $step.log) {
            foreach ($last in @(Get-LastLines (Join-Path $RunDir $step.log) 3)) { $lines.Add("              | $last") }
        }
    }
    if ($Problems.Count -gt 0) {
        $lines.Add('')
        $lines.Add('problems:')
        foreach ($problem in $Problems) { $lines.Add("  - $problem") }
    }
    $lines.Add('')
    $lines.Add('Details: system.txt, tests\<name>.log, emulator-*.log, imports.txt, session.log, console.txt.')
    Write-Text (Join-Path $RunDir 'summary.txt') (($lines -join $NewLine) + $NewLine)
}

# The summary and console.txt so far, written after every step: a results folder left behind (the
# window closed, the PC restarted) still has them. The finally block writes the final ones.
function Save-Progress {
    try {
        Write-Summary 'RUNNING'
        Write-Text (Join-Path $RunDir 'console.txt') $ConsoleText.ToString()
    } catch { }
}

# --- Main -----------------------------------------------------------------------------------------

function Invoke-Preflight([object[]]$Arguments) {
    $script:Options = Read-Arguments $Arguments
    $opts = $script:Options
    if ($opts.Help) {
        Show-Usage
        $script:ExitCode = 0
        return
    }
    $bootSeconds = 300
    if ($null -ne $opts.BootSeconds) {
        $n = 0
        if (-not [int]::TryParse($opts.BootSeconds, [System.Globalization.NumberStyles]::Integer, $Invariant, [ref]$n) -or $n -lt 0) {
            throw (New-UsageError "-BootSeconds takes a whole number of seconds, 0 or more (0: until you close the emulator), not '$($opts.BootSeconds)'")
        }
        $bootSeconds = $n
    }
    if ($opts.Quick -and $opts.Only.Count -gt 0) { throw (New-UsageError 'use -Quick or -Only, not both') }
    $packageDir = Join-Path $ScriptDir 'gpu-tests'
    $testsJson = Join-Path $packageDir 'tests.json'
    $collect = Join-Path $ScriptDir 'collect_logs.ps1'
    $game = $null
    $emulator = $null
    if ($opts.Game) {
        $script:Mode = 'game'
        # cmd passes a trailing \" as a quote: "D:\GTA 6\" arrives as D:\GTA 6".
        $game = Get-FullPath ($opts.Game.Trim().Trim('"'))
        if (-not (Test-Path -LiteralPath $game)) { throw (New-UsageError "-Game: $game does not exist") }
        if ($opts.Emulator) {
            $emulator = Get-FullPath ($opts.Emulator.Trim().Trim('"'))
            if (Test-Path -LiteralPath $emulator -PathType Container) { $emulator = Join-Path $emulator 'kyty_emulator.exe' }
        } else {
            $name = 'kyty_emulator'
            if ($OnWindows) { $name = 'kyty_emulator.exe' }
            $emulator = Join-Path (Split-Path -Parent $ScriptDir) $name
        }
        if (-not (Test-Path -LiteralPath $emulator -PathType Leaf)) {
            throw (New-UsageError "the emulator is not at $emulator. Run preflight from the preflight folder of the extracted build, or pass -Emulator <kyty_emulator.exe>")
        }
    }
    $runTests = (-not $opts.Game) -or $opts.WithTests
    $manifest = $null
    if (Test-Path -LiteralPath $testsJson -PathType Leaf) {
        $manifest = [System.IO.File]::ReadAllText($testsJson, $Utf8) | ConvertFrom-Json
        if ((Get-Field $manifest 'format' 0) -ne 1) {
            throw (New-UsageError "$testsJson has format $(Get-Field $manifest 'format' '?'); this preflight.ps1 reads format 1")
        }
        $source = Get-Field $manifest 'source'
        if ($null -ne $source) { foreach ($property in $source.PSObject.Properties) { $Package[$property.Name] = [string]$property.Value } }
    } elseif ($runTests) {
        throw (New-UsageError "$testsJson is missing. Extract the whole zip first (right-click it, Extract All), then run preflight.cmd from the extracted preflight folder")
    }
    $selected = @()
    if ($runTests) {
        $all = @(Get-Field $manifest 'tests' @())
        if ($opts.Only.Count -gt 0) {
            $unknown = New-Object System.Collections.Generic.List[string]
            $picked = New-Object System.Collections.Generic.List[object]
            foreach ($name in $opts.Only) {
                $hit = @($all | Where-Object { $_.name -eq $name })
                if ($hit.Count -eq 0) { $unknown.Add($name) } elseif (-not $picked.Contains($hit[0])) { $picked.Add($hit[0]) }
            }
            if ($unknown.Count -gt 0) {
                throw (New-UsageError "no test named $($unknown -join ', ') in tests.json. The tests: $((@($all | ForEach-Object { $_.name })) -join ', ')")
            }
            $selected = $picked.ToArray()
        } elseif ($opts.Quick) {
            $selected = @($all | Where-Object { [bool](Get-Field $_ 'quick' $false) })
        } else {
            $selected = $all
        }
        if ($selected.Count -eq 0) { throw (New-UsageError "$testsJson selects no tests") }
    }

    $outDir = $ScriptDir
    if ($opts.OutDir) { $outDir = Get-FullPath ($opts.OutDir.Trim().Trim('"')) }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $baseName = "kyty-preflight-$ComputerName-$($StartedAt.ToString('yyyyMMdd-HHmmss', $Invariant))"
    $folderName = $baseName
    $n = 2
    while ((Test-Path -LiteralPath (Join-Path $outDir $folderName)) -or (Test-Path -LiteralPath (Join-Path $outDir "$folderName.zip"))) {
        $folderName = "$baseName-$n"
        $n++
    }
    $script:RunDir = Join-Path $outDir $folderName
    New-Item -ItemType Directory -Path $script:RunDir | Out-Null

    try { $Host.UI.RawUI.WindowTitle = 'KytyPS5 preflight' } catch { }
    $title = 'KytyPS5 preflight'
    if ($Package.Contains('commit')) {
        $commit = $Package['commit']
        if ($commit.Length -gt 10) { $commit = $commit.Substring(0, 10) }
        $title += " ($commit"
        if ($Package.Contains('run_id')) { $title += ", CI run $($Package['run_id'])" }
        $title += ')'
    }
    Say $title 'Cyan'
    Say "Results: $script:RunDir"
    if ($CtrlCAsInput) { Say 'Ctrl+C stops early; what ran so far is still zipped.' }
    if (-not $opts.Game -and ($null -ne $opts.BootSeconds -or $opts.ExtraArgs.Count -gt 0 -or $opts.Emulator -or $opts.WithTests)) {
        Add-Problem '-BootSeconds, -ExtraArgs, -Emulator and -WithTests only work with -Game; running the GPU tests'
    }
    if ($opts.Game -and -not $opts.WithTests -and ($opts.Quick -or $opts.Only.Count -gt 0)) {
        Add-Problem '-Quick and -Only pick GPU tests, which -Game runs only with -WithTests'
    }
    if ($opts.Game -and (Test-Path -LiteralPath $game -PathType Container) -and -not (Test-Path -LiteralPath (Join-Path $game 'eboot.bin'))) {
        Add-Problem "$game has no eboot.bin: the emulator will probably not load it"
    }
    Initialize-Native
    if ($NativeReady) {
        # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX. Child processes
        # inherit it: a missing DLL or a crash then ends the process with its status code, instead
        # of opening a dialog box that waits for a click until the time limit.
        [void][KytyPreflight.Native]::SetErrorMode(0x8003)
        # A PC that goes to sleep during a test fails that test for nothing (a lost device, or the
        # time limit). The finally block undoes both of these.
        [KytyPreflight.Native]::KeepAwake($true)
        $script:InputMode = [KytyPreflight.Native]::QuickEditOff()
    }
    Say 'Collecting system information ...'
    $system = @(Get-SystemInfo)
    Write-Text (Join-Path $script:RunDir 'system.txt') (($system -join $NewLine) + $NewLine)
    foreach ($line in $system) {
        if ($line -match '^(os|cpu|gpu|vram \(registry\)): ') { Say "  $line" 'DarkGray' }
    }
    Stop-OnKey

    if ($opts.Game) {
        $tests = @()
        if ($runTests) { $tests = $selected }
        Invoke-Game $emulator $game $opts.ExtraArgs.ToArray() $bootSeconds $collect $manifest $tests $packageDir
    } else {
        Invoke-Tests $manifest $selected $packageDir
    }
    $script:Completed = $true
    $script:ExitCode = 0
    if (@($Steps | Where-Object { $_.status -ne 'PASS' -and $_.status -ne 'RAN' }).Count -gt 0) { $script:ExitCode = 1 }
}

try {
    # Ctrl+C as a key while this window has a console: Test-StopKey sees it between waits.
    try {
        if (-not [Console]::IsInputRedirected) {
            [Console]::TreatControlCAsInput = $true
            $CtrlCAsInput = $true
        }
    } catch { }
    try {
        $null = Invoke-Preflight (Get-ScriptArguments $args)
    } catch {
        $Failure = $_.Exception.Message
        $ExitCode = 2
        if ($StopRequested) {
            $Failure = $null
        } elseif ($UsageProblem) {
            Say "preflight: $Failure" 'Red'
            Show-Usage
        } else {
            Say "preflight could not finish: $Failure" 'Red'
            Say "  at line $($_.InvocationInfo.ScriptLineNumber): $(([string]$_.InvocationInfo.Line).Trim())" 'DarkGray'
        }
    }
} finally {
    if ($null -ne $Running) {
        # Stopped early, or failed while a child ran: stop it and keep what it wrote.
        $child = $Running
        # A booting game gets the same chance to close its window and write out its logs.
        $grace = 0
        if ($null -ne $RunningStep -and $RunningStep.kind -eq 'boot') { $grace = 15 }
        try {
            [void](Stop-Child $child $grace)
            $code = Complete-Child $child
            Write-Text $child.Log "$NewLine--- stopped early by preflight, $(Describe-ExitCode $code) ---$NewLine" -Append
            if ($null -ne $RunningStep) {
                $RunningStep.status = 'INTERRUPTED'
                Set-Exit $RunningStep $code
            }
        } catch { }
    }
    if ($null -ne $RunDir -and (Test-Path -LiteralPath $RunDir)) {
        $result = 'ERROR'
        $ran = @($Steps | Where-Object { $_.status -eq 'RAN' }).Count
        if ($Completed) {
            $result = 'PASS'
            if ($ran -gt 0) { $result = 'RAN' }
            if ($ExitCode -ne 0) { $result = 'FAIL' }
        } elseif ($null -eq $Failure) {
            $result = 'INTERRUPTED'
            $ExitCode = 1
            Say ''
            Say 'Stopped early; zipping what ran so far.' 'Yellow'
        } else {
            $Problems.Add((Hide "preflight could not finish: $Failure"))
        }
        $zipPath = "$RunDir.zip"
        try {
            if ($Completed) {
                $passed = @($Steps | Where-Object { $_.status -eq 'PASS' }).Count
                $text = "$passed of $($Steps.Count) steps passed."
                $color = 'Yellow'
                if ($ExitCode -eq 0) { $color = 'Green' }
                if ($ran -gt 0) {
                    # Not "it works": the boot ran, and nothing here can tell whether the game did.
                    $text += ' The boot ran without crashing; whether the game works shows only in its logs.'
                    if ($ExitCode -eq 0) { $color = 'Cyan' }
                }
                Say ''
                Say $text $color
            }
            Write-Summary $result
            Write-Text (Join-Path $RunDir 'console.txt') $ConsoleText.ToString()
            Write-Zip $RunDir $zipPath @('emulator')
            Say ''
            Say "Results: $zipPath" 'Cyan'
            Say 'Send this file to Claude' 'Cyan'
        } catch {
            Say "could not write the zip: $($_.Exception.Message)" 'Red'
            Say "Send the folder $RunDir instead (zip it yourself)." 'Yellow'
            $ExitCode = 2
        }
    }
    if ($NativeReady) {
        try { [KytyPreflight.Native]::KeepAwake($false) } catch { }
        # Before TreatControlCAsInput goes back below: the saved mode is from after it was set.
        try { [KytyPreflight.Native]::RestoreInputMode($InputMode) } catch { }
    }
    # Last, so that a second Ctrl+C cannot cut the zip short. Keys pressed meanwhile would end
    # preflight.cmd's pause before the zip's name can be read.
    if ($CtrlCAsInput) {
        try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch { }
        try { [Console]::TreatControlCAsInput = $false } catch { }
    }
}
exit $ExitCode
