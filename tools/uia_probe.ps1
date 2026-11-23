Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName WindowsBase

$sig = @'
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
}
'@
Add-Type -TypeDefinition $sig

$fg = [W]::GetForegroundWindow()
$sb = New-Object System.Text.StringBuilder 512
[void][W]::GetClassNameW($fg, $sb, 512)
$cls = $sb.ToString()
[void][W]::GetWindowTextW($fg, $sb, 512)
$ttl = $sb.ToString()
$wpid = 0
$tid = [W]::GetWindowThreadProcessId($fg, [ref]$wpid)
Write-Output "FG hwnd=$fg class=$cls title=$ttl pid=$wpid tid=$tid"

$el = [System.Windows.Automation.AutomationElement]::FocusedElement
if (-not $el) { Write-Output "no focused element"; exit }
Write-Output ("EL pid=" + $el.Current.ProcessId + " class=" + $el.Current.ClassName + " type=" + $el.Current.ControlType.ProgrammaticName)
Write-Output ("EL name=" + $el.Current.Name)
Write-Output ("EL rect=" + $el.Current.BoundingRectangle)
Write-Output ("EL nativeHwnd=" + $el.Current.NativeWindowHandle)
Write-Output ("EL hasKeyboardFocus=" + $el.Current.HasKeyboardFocus)

$tp = $null
if ($el.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$tp)) {
    Write-Output "TEXTPATTERN ok"
    try {
        $sel = $tp.GetSelection()
        Write-Output ("selection count=" + $sel.Count)
        foreach ($r in $sel) {
            $brs = $r.GetBoundingRectangles()
            $parts = @()
            foreach ($q in $brs) { $parts += ("{0},{1},{2}x{3}" -f [int]$q.Left, [int]$q.Top, [int]$q.Width, [int]$q.Height) }
            Write-Output ("  sel rects count=" + $brs.Count + " : " + ($parts -join ';'))
        }
    } catch { Write-Output ("selection error: " + $_.Exception.Message) }
} else {
    Write-Output "TEXTPATTERN none"
}

$vp = $null
if ($el.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$vp)) {
    Write-Output ("VALUEPATTERN value=" + $vp.Current.Value)
} else { Write-Output "VALUEPATTERN none" }

try {
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $p = $el
    for ($i = 0; $i -lt 40; $i++) {
        $n = $walker.GetParent($p)
        if (-not $n) { break }
        if ($n -eq [System.Windows.Automation.AutomationElement]::RootElement) { break }
        $p = $n
    }
    Write-Output ("TOP pid=" + $p.Current.ProcessId + " class=" + $p.Current.ClassName + " nativeHwnd=" + $p.Current.NativeWindowHandle)
} catch { Write-Output ("walker error: " + $_.Exception.Message) }
