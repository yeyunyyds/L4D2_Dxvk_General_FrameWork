param([Parameter(Mandatory=$true)][string]$LogDirectory)
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object Windows.Forms.Form
$form.Text = 'GF Frame Generation Monitor'
$form.Size = New-Object Drawing.Size(450,335)
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
  'time_utc,input_fps,native_accept_fps,layer_input_fps,layer_output_fps,output_ratio,accept_ratio,host_work_ms,fence_wait_ms,motion_pixels,native_accepted,native_released,native_dispatches,frame_generation_active,separate_ui_active,status' | Set-Content -Encoding UTF8 $csv
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

function Find-EventLog([string]$directory) {
  $direct = Join-Path $directory 'fg-events.jsonl'
  if (Test-Path $direct) { return $direct }
  $candidate = Get-ChildItem -LiteralPath $directory -Filter 'fg-events.jsonl' -File -Recurse |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  if ($candidate) { return $candidate.FullName }
  return $null
}

function Read-QpcFrequency([string]$path) {
  if (-not (Test-Path $path)) { return $null }
  $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
  try {
    $take = [Math]::Min($stream.Length,64KB)
    $buffer = New-Object byte[] $take
    $read = $stream.Read($buffer,0,$take)
    $text = [Text.Encoding]::UTF8.GetString($buffer,0,$read)
  } finally { $stream.Dispose() }
  foreach ($line in ($text -split "`r?`n")) {
    if ($line.StartsWith('{')) {
      try {
        $event = $line | ConvertFrom-Json
        if ($event.event -eq 'session' -and [double]$event.qpc_frequency -gt 0) {
          return [double]$event.qpc_frequency
        }
      } catch {}
    }
  }
  return $null
}

function Read-LayerState([string]$path) {
  if (-not (Test-Path $path)) { return $null }
  $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
  try {
    $take = [int][Math]::Min($stream.Length,8MB)
    $buffer = New-Object byte[] $take
    $read = $stream.Read($buffer,0,$buffer.Length)
    $text = [Text.Encoding]::UTF8.GetString($buffer,0,$read)
  } finally { $stream.Dispose() }
  try { $document = $text | ConvertFrom-Json } catch { return $null }
  $live = $document.swapchains.PSObject.Properties.Value |
    Where-Object { $_.live -eq $true } | Select-Object -Last 1
  if (-not $live) { return $null }
  return [PSCustomObject]@{
    InputFps = [double]$live.input_present_rate_hz_ema
    OutputFps = [double]$live.estimated_output_rate_hz
    OutputRatio = [double]$live.output_presents_per_input
    NativeOutputRatio = [double]$live.native_output_presents_per_input
    NativeInputs = [uint64]$live.native_input_presents
    NativeOutputs = [uint64]$live.native_output_presents
    NativeDispatches = [uint64]$live.native_interpolation_dispatches
    NativeBound = $live.native_frame_inputs_bound -eq $true
    Active = $live.frame_generation_active -eq $true
    SeparateUi = $live.separate_ui_composition_active -eq $true
  }
}

$script:EventPath = $null
$script:QpcFrequency = $null
$script:LayerState = $null
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
  if (-not $script:EventPath -or -not (Test-Path $script:EventPath)) {
    $script:EventPath = Find-EventLog $LogDirectory
  }
  if (-not $script:EventPath) { return }
  $events = @(Read-RecentEvents $script:EventPath)
  $session = $events | Where-Object event -eq 'session' | Select-Object -Last 1
  if ($session -and [double]$session.qpc_frequency -gt 0) {
    $script:QpcFrequency = [double]$session.qpc_frequency
  } elseif (-not $script:QpcFrequency) {
    $script:QpcFrequency = Read-QpcFrequency $script:EventPath
  }
  $frames = @($events | Where-Object event -eq 'frame_metrics' | Select-Object -Last 240)
  if ($frames.Count -lt 2 -or -not $script:QpcFrequency) { return }
  $first = $frames[0]; $last = $frames[-1]
  $seconds = ([double]$last.arrival_qpc - [double]$first.arrival_qpc) / $script:QpcFrequency
  if ($seconds -le 0) { return }
  $inputFps = ($frames.Count - 1) / $seconds
  $acceptedDelta = [double]$last.native_accepted_total - [double]$first.native_accepted_total
  $nativeFps = [Math]::Max(0,$acceptedDelta / $seconds)
  $submittedDelta = [double]$last.native_submitted_total - [double]$first.native_submitted_total
  $ratio = if ($submittedDelta -gt 0) { $acceptedDelta / $submittedDelta } else { 0 }
  $released = [uint64]$last.native_released_total
  $accepted = [uint64]$last.native_accepted_total
  $layer = Read-LayerState (Join-Path $LogDirectory 'asteria-layer.json')
  if ($layer) { $script:LayerState = $layer } else { $layer = $script:LayerState }
  $status = if ($last.enabled -ne $true) { 'DISABLED' }
            elseif ($acceptedDelta -le 0) { 'NO NATIVE INPUT' }
            elseif ($ratio -lt 0.95) { 'INPUT DROPS' }
            elseif ($released -gt $accepted) { 'LEASE ERROR' }
            elseif (-not $layer -or -not $layer.NativeBound) { 'LAYER WAIT' }
            elseif (-not $layer.Active -or $layer.NativeDispatches -le 0) { 'LAYER NOT DISPATCHING' }
            elseif ($layer.NativeInputs -gt 30 -and $layer.NativeOutputRatio -lt 1.8) { 'OUTPUT BELOW 2X' }
            else { 'NATIVE FG ACTIVE' }
  $label.ForeColor = if ($status -eq 'NATIVE FG ACTIVE') {[Drawing.Color]::LightGreen} else {[Drawing.Color]::Orange}
  $outputRatio = if ($layer) { $layer.NativeOutputRatio } else { 0 }
  $layerInput = $nativeFps
  $layerOutput = $nativeFps * $outputRatio
  $nativeDispatches = if ($layer) { $layer.NativeDispatches } else { 0 }
  $layerActive = if ($layer -and $layer.Active) { 1 } else { 0 }
  $separateUi = if ($layer -and $layer.SeparateUi) { 1 } else { 0 }
  $colorSource = if ($separateUi) { 'HOST HUDLESS + UI' } else { 'SWAPCHAIN PRESENT' }
  $label.Text = ("{0}`r`n`r`nSource / accepted {1,6:N1} / {2,6:N1} FPS`r`nLayer in / output {3,6:N1} / {4,6:N1} FPS`r`nOutput / accept    {5,6:N2}x / {6,6:P1}`r`nHost / fence      {7,5:N2} / {8,5:N2} ms`r`nMotion pixels     {9,10:N0}`r`nLeases A/R        {10} / {11}`r`nNative dispatches {12}`r`nColor source      {13}`r`n`r`nF8 toggles FG. Record about 20 seconds in each mode." -f
    $status,$inputFps,$nativeFps,$layerInput,$layerOutput,$outputRatio,$ratio,[double]$last.host_work_ms,[double]$last.fence_wait_ms,[double]$last.motion_interpolated_pixels,$accepted,$released,$nativeDispatches,$colorSource)
  $row = [string]::Format([Globalization.CultureInfo]::InvariantCulture,
    '{0:o},{1:F3},{2:F3},{3:F3},{4:F3},{5:F6},{6:F6},{7:F4},{8:F4},{9},{10},{11},{12},{13},{14},{15}',
    @([DateTime]::UtcNow,$inputFps,$nativeFps,$layerInput,$layerOutput,$outputRatio,$ratio,[double]$last.host_work_ms,[double]$last.fence_wait_ms,[uint64]$last.motion_interpolated_pixels,$accepted,$released,$nativeDispatches,$layerActive,$separateUi,$status))
  Add-Content -Encoding UTF8 -Path $csv -Value $row
})
$form.Add_FormClosed({ $timer.Stop() })
$timer.Start()
[void]$form.ShowDialog()
