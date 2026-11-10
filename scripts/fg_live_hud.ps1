param([Parameter(Mandatory=$true)][string]$LogDirectory)
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object Windows.Forms.Form
$form.Text = 'GF Frame Generation Monitor'
$form.Size = New-Object Drawing.Size(430,245)
$form.StartPosition = 'Manual'
$form.Location = New-Object Drawing.Point(20,20)
$form.TopMost = $true
$form.FormBorderStyle = 'FixedToolWindow'
$form.BackColor = [Drawing.Color]::FromArgb(24,24,27)
$label = New-Object Windows.Forms.Label
$label.Dock = 'Fill'
$label.Padding = New-Object Windows.Forms.Padding -ArgumentList 14,12,8,8
$label.Font = New-Object Drawing.Font('Consolas',11)
$label.ForeColor = [Drawing.Color]::Gainsboro
$label.Text = "Waiting for Host frame data...`r`n$LogDirectory"
$form.Controls.Add($label)

$csv = Join-Path $LogDirectory 'fg-live-comparison.csv'
if (-not (Test-Path $csv)) {
  'time_utc,input_fps,native_accept_fps,expected_output_fps,accept_ratio,host_work_ms,fence_wait_ms,motion_pixels,native_accepted,native_released,status' | Set-Content -Encoding UTF8 $csv
}

function Read-RecentEvents([string]$path) {
  if (-not (Test-Path $path)) { return @() }
  $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
  try {
    $length = $stream.Length
    $take = [Math]::Min($length,1MB)
    $stream.Seek(-$take,[IO.SeekOrigin]::End) | Out-Null
    $buffer = New-Object byte[] $take
    $read = $stream.Read($buffer,0,$take)
    $text = [Text.Encoding]::UTF8.GetString($buffer,0,$read)
  } finally { $stream.Dispose() }
  $lines = $text -split "`r?`n"
  if ($take -lt $length) { $lines = $lines | Select-Object -Skip 1 }
  $events = @()
  foreach ($line in $lines) {
    if ($line.StartsWith('{')) {
      try { $events += ($line | ConvertFrom-Json) } catch {}
    }
  }
  return $events
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
  $path = Join-Path $LogDirectory 'fg-events.jsonl'
  $events = @(Read-RecentEvents $path)
  $session = $events | Where-Object event -eq 'session' | Select-Object -Last 1
  $frames = @($events | Where-Object event -eq 'frame_metrics' | Select-Object -Last 240)
  if ($frames.Count -lt 2 -or -not $session.qpc_frequency) { return }
  $first = $frames[0]; $last = $frames[-1]
  $seconds = ([double]$last.arrival_qpc - [double]$first.arrival_qpc) / [double]$session.qpc_frequency
  if ($seconds -le 0) { return }
  $inputFps = ($frames.Count - 1) / $seconds
  $acceptedDelta = [double]$last.native_accepted_total - [double]$first.native_accepted_total
  $nativeFps = [Math]::Max(0,$acceptedDelta / $seconds)
  $expected = $nativeFps * 2
  $submittedDelta = [double]$last.native_submitted_total - [double]$first.native_submitted_total
  $ratio = if ($submittedDelta -gt 0) { $acceptedDelta / $submittedDelta } else { 0 }
  $released = [uint64]$last.native_released_total
  $accepted = [uint64]$last.native_accepted_total
  $status = if ($last.enabled -ne $true) { 'DISABLED' }
            elseif ($acceptedDelta -le 0) { 'NO NATIVE INPUT' }
            elseif ($ratio -lt 0.95) { 'INPUT DROPS' }
            elseif ($released -gt $accepted) { 'LEASE ERROR' }
            else { 'NATIVE FG ACTIVE' }
  $label.ForeColor = if ($status -eq 'NATIVE FG ACTIVE') {[Drawing.Color]::LightGreen} else {[Drawing.Color]::Orange}
  $label.Text = ("{0}`r`n`r`nNative input     {1,7:N1} FPS`r`nFG accepted      {2,7:N1} FPS`r`nExpected presents {3,7:N1} FPS`r`nAccept ratio      {4,7:P1}`r`nHost / fence      {5,5:N2} / {6,5:N2} ms`r`nMotion pixels     {7,10:N0}`r`nLeases A/R        {8} / {9}`r`n`r`nActual scanout is verified in the result log." -f
    $status,$inputFps,$nativeFps,$expected,$ratio,[double]$last.host_work_ms,[double]$last.fence_wait_ms,[double]$last.motion_interpolated_pixels,$accepted,$released)
  $row = '{0:o},{1:F3},{2:F3},{3:F3},{4:F6},{5:F4},{6:F4},{7},{8},{9},{10}' -f
    [DateTime]::UtcNow,$inputFps,$nativeFps,$expected,$ratio,[double]$last.host_work_ms,[double]$last.fence_wait_ms,[uint64]$last.motion_interpolated_pixels,$accepted,$released,$status
  Add-Content -Encoding UTF8 -Path $csv -Value $row
})
$form.Add_FormClosed({ $timer.Stop() })
$timer.Start()
[void]$form.ShowDialog()
