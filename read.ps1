# Capture DK VCOM ports to log files for a while.
#   .\read.ps1 -Ports COM151,COM152 -Seconds 60
param(
    [string[]]$Ports = @('COM151', 'COM152'),
    [int]$Seconds = 60,
    [string]$OutDir = $PSScriptRoot
)

$serials = @{}
foreach ($p in $Ports) {
    $s = New-Object System.IO.Ports.SerialPort $p, 115200, 'None', 8, 'One'
    $s.ReadTimeout = 50
    $s.Open()
    $serials[$p] = $s
    Set-Content -Path (Join-Path $OutDir "$p.log") -Value '' -Encoding utf8
}

$end = (Get-Date).AddSeconds($Seconds)
try {
    while ((Get-Date) -lt $end) {
        foreach ($p in $Ports) {
            $data = $serials[$p].ReadExisting()
            if ($data) {
                Add-Content -Path (Join-Path $OutDir "$p.log") -Value $data -NoNewline -Encoding utf8
            }
        }
        Start-Sleep -Milliseconds 50
    }
} finally {
    foreach ($s in $serials.Values) { $s.Close() }
}
