Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$e = [System.Windows.Automation.AutomationElement]::FocusedElement
if ($null -eq $e) { "no focused element"; exit }
"focused: " + $e.Current.ControlType.ProgrammaticName + " name=" + $e.Current.Name

try {
    $tp = $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
    $sel = $tp.GetSelection()
    "selection count: " + $sel.Length
    foreach ($r in $sel) {
        $rects = $r.GetBoundingRectangles()
        "  rects: " + $rects.Length + " -> " + ($rects -join ",")
        "  text: '" + $r.GetText(64) + "'"
    }
} catch {
    "TextPattern failed: " + $_.Exception.Message
}
