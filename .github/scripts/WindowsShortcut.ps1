# Dot-source this file to read .lnk files without WScript's ANSI path boundary.
# The reader never resolves, saves, or launches the shortcut.
function Get-WindowsShortcut {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [ValidateNotNullOrEmpty()]
        [string]$LiteralPath
    )

    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Get-WindowsShortcut requires Windows.'
    }
    if (-not (Test-Path -LiteralPath $LiteralPath)) {
        throw "Windows shortcut does not exist: $LiteralPath"
    }
    $shortcutFile = Get-Item -LiteralPath $LiteralPath -ErrorAction Stop
    if ($shortcutFile.PSIsContainer -or
        $shortcutFile.PSProvider.Name -ne 'FileSystem') {
        throw "Windows shortcut is not a file: $LiteralPath"
    }

    if (-not ('Yanami.Ci.WindowsShortcutReader' -as [type])) {
        Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace Yanami.Ci
{
    // Only the initial IShellLinkW vtable slots needed by this read-only tool.
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
    }

    public sealed class WindowsShortcutInfo
    {
        public string TargetPath { get; private set; }
        public string WorkingDirectory { get; private set; }

        internal WindowsShortcutInfo(string targetPath, string workingDirectory)
        {
            TargetPath = targetPath;
            WorkingDirectory = workingDirectory;
        }
    }

    public static class WindowsShortcutReader
    {
        public static WindowsShortcutInfo Read(string path)
        {
            object instance = null;
            try
            {
                Type shellLinkType = Type.GetTypeFromCLSID(
                    new Guid("00021401-0000-0000-C000-000000000046"), true);
                instance = Activator.CreateInstance(shellLinkType);
                // STGM_READ. Do not call Resolve or Save: this is inspection.
                ((IPersistFile)instance).Load(path, 0);
                IShellLinkW link = (IShellLinkW)instance;
                var target = new StringBuilder(32768);
                var directory = new StringBuilder(32768);
                const uint SLGP_RAWPATH = 0x00000004;
                Marshal.ThrowExceptionForHR(link.GetPath(
                    target, target.Capacity, IntPtr.Zero, SLGP_RAWPATH));
                Marshal.ThrowExceptionForHR(link.GetWorkingDirectory(
                    directory, directory.Capacity));
                return new WindowsShortcutInfo(
                    target.ToString(), directory.ToString());
            }
            finally
            {
                // Both COM interfaces share this RCW; release it exactly once.
                if (instance != null && Marshal.IsComObject(instance))
                    Marshal.FinalReleaseComObject(instance);
            }
        }
    }
}
'@
    }

    try {
        $nativeShortcut = [Yanami.Ci.WindowsShortcutReader]::Read(
            $shortcutFile.FullName)
        [pscustomobject]@{
            LiteralPath = $shortcutFile.FullName
            TargetPath = $nativeShortcut.TargetPath
            WorkingDirectory = $nativeShortcut.WorkingDirectory
        }
    } catch {
        $detail = $_.Exception.GetBaseException().Message
        throw "Could not read Windows shortcut '$($shortcutFile.FullName)' through IShellLinkW: $detail"
    }
}
