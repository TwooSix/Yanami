$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Windows shortcut regression tests require Windows.'
}
. (Join-Path $PSScriptRoot 'WindowsShortcut.ps1')

$assertions = 0
function Assert-Equal {
    param($Actual, $Expected, [string]$Label)
    $script:assertions++
    if ($Actual -cne $Expected) {
        throw "$Label expected '$Expected', got '$Actual'."
    }
}
function Assert-Throws {
    param([scriptblock]$Action, [string]$Expected, [string]$Label)
    $script:assertions++
    try {
        & $Action
    } catch {
        if (-not $_.Exception.Message.Contains(
                $Expected, [StringComparison]::Ordinal)) {
            throw "$Label returned an unexpected error: $($_.Exception.Message)"
        }
        return
    }
    throw "$Label did not throw."
}

# Independent wide-interface fixture writer. It writes only inside the fresh
# test directory below and never creates a real desktop/Start-menu shortcut.
if (-not ('Yanami.Ci.Tests.WindowsShortcutFixture' -as [type])) {
    Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace Yanami.Ci.Tests
{
    [ComImport, Guid("000214F9-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IShellLinkW
    {
        [PreserveSig] int GetPath(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path,
            int count, IntPtr findData, uint flags);
        [PreserveSig] int GetIDList(out IntPtr idList);
        [PreserveSig] int SetIDList(IntPtr idList);
        [PreserveSig] int GetDescription(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder description,
            int count);
        [PreserveSig] int SetDescription(
            [MarshalAs(UnmanagedType.LPWStr)] string description);
        [PreserveSig] int GetWorkingDirectory(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder directory,
            int count);
        [PreserveSig] int SetWorkingDirectory(
            [MarshalAs(UnmanagedType.LPWStr)] string directory);
        [PreserveSig] int GetArguments(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder arguments,
            int count);
        [PreserveSig] int SetArguments(
            [MarshalAs(UnmanagedType.LPWStr)] string arguments);
        [PreserveSig] int GetHotkey(out short hotkey);
        [PreserveSig] int SetHotkey(short hotkey);
        [PreserveSig] int GetShowCmd(out int showCommand);
        [PreserveSig] int SetShowCmd(int showCommand);
        [PreserveSig] int GetIconLocation(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder iconPath,
            int count, out int iconIndex);
        [PreserveSig] int SetIconLocation(
            [MarshalAs(UnmanagedType.LPWStr)] string iconPath, int iconIndex);
        [PreserveSig] int SetRelativePath(
            [MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
        [PreserveSig] int Resolve(IntPtr window, uint flags);
        [PreserveSig] int SetPath(
            [MarshalAs(UnmanagedType.LPWStr)] string path);
    }

    public static class WindowsShortcutFixture
    {
        [DllImport("kernel32.dll")]
        public static extern uint GetACP();

        public static void Create(string shortcut, string target, string directory)
        {
            if (File.Exists(shortcut) || Directory.Exists(shortcut))
                throw new IOException("Refusing to overwrite a shortcut fixture.");
            object instance = null;
            try
            {
                instance = Activator.CreateInstance(Type.GetTypeFromCLSID(
                    new Guid("00021401-0000-0000-C000-000000000046"), true));
                IShellLinkW link = (IShellLinkW)instance;
                Marshal.ThrowExceptionForHR(link.SetPath(target));
                Marshal.ThrowExceptionForHR(link.SetWorkingDirectory(directory));
                Marshal.ThrowExceptionForHR(link.SetIconLocation(target, 0));
                Marshal.ThrowExceptionForHR(link.SetDescription("Yanami fixture"));
                ((IPersistFile)instance).Save(shortcut, true);
            }
            finally
            {
                if (instance != null && Marshal.IsComObject(instance))
                    Marshal.FinalReleaseComObject(instance);
            }
        }
    }
}
'@
}

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildRoot = Join-Path $workspace 'build'
if (Test-Path -LiteralPath $buildRoot) {
    $buildItem = Get-Item -LiteralPath $buildRoot -Force
    if (-not $buildItem.PSIsContainer -or
        ($buildItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "The test build root must be a real directory: $buildRoot"
    }
} else {
    New-Item -ItemType Directory -Path $buildRoot | Out-Null
}
$testRoot = Join-Path $buildRoot (
    'windows-shortcut-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
Write-Host "Shortcut fixtures retained in: $testRoot"
Write-Host "Windows ANSI code page: $([Yanami.Ci.Tests.WindowsShortcutFixture]::GetACP())"

# The supplementary-plane character is outside legacy ACPs. On UTF-8 ACP
# systems it remains a Unicode round-trip case; no WScript failure is assumed.
$testCases = @(
    @{ Name = 'ASCII'; Directory = 'Yanami desktop smoke' },
    @{ Name = 'CJK'; Directory = 'Yanami 安装 desktop smoke' },
    @{ Name = 'Unicode'; Directory = ('Yanami 安装 desktop smoke ' + [char]::ConvertFromUtf32(0x1f9ea)) }
)
foreach ($case in $testCases) {
    $workingDirectory = Join-Path (Join-Path $testRoot $case.Directory) 'current'
    New-Item -ItemType Directory -Path $workingDirectory | Out-Null
    $targetPath = Join-Path $workingDirectory 'Yanami.exe'
    # Empty inert file, never executed. Its existence exercises Shell path IO.
    New-Item -ItemType File -Path $targetPath | Out-Null
    $shortcutPath = Join-Path (Split-Path -Parent $workingDirectory) 'Yanami [literal].lnk'
    [Yanami.Ci.Tests.WindowsShortcutFixture]::Create(
        $shortcutPath, $targetPath, $workingDirectory)
    $hashBefore = (Get-FileHash -LiteralPath $shortcutPath -Algorithm SHA256).Hash

    $actual = Get-WindowsShortcut -LiteralPath $shortcutPath
    Assert-Equal $actual.LiteralPath $shortcutPath "$($case.Name) literal file"
    Assert-Equal $actual.TargetPath $targetPath "$($case.Name) target"
    Assert-Equal $actual.WorkingDirectory $workingDirectory "$($case.Name) working directory"
    Assert-Equal (Get-FileHash -LiteralPath $shortcutPath -Algorithm SHA256).Hash `
        $hashBefore "$($case.Name) read does not modify shortcut"
    Write-Host "PASS $($case.Name): native Unicode target and working directory match exactly."
}

$missingPath = Join-Path $testRoot 'missing.lnk'
Assert-Throws { Get-WindowsShortcut -LiteralPath $missingPath } `
    'Windows shortcut does not exist:' 'Missing shortcut'
Assert-Equal (Test-Path -LiteralPath $missingPath) $false `
    'Reading a missing shortcut does not create it'
Assert-Throws { Get-WindowsShortcut -LiteralPath $testRoot } `
    'Windows shortcut is not a file:' 'Directory instead of shortcut'
$invalidPath = Join-Path $testRoot 'invalid [literal].lnk'
[IO.File]::WriteAllText($invalidPath, 'This is not a Shell Link file.')
$invalidHash = (Get-FileHash -LiteralPath $invalidPath -Algorithm SHA256).Hash
Assert-Throws { Get-WindowsShortcut -LiteralPath $invalidPath } `
    'through IShellLinkW:' 'Invalid shortcut contents'
Assert-Equal (Get-FileHash -LiteralPath $invalidPath -Algorithm SHA256).Hash `
    $invalidHash 'Reading an invalid shortcut does not modify it'

Write-Host "Windows shortcut tests passed ($assertions assertions). No binaries were executed and no installed application state was accessed."
