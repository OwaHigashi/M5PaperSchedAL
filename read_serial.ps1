param([int]$Seconds = 30, [string]$Port = "COM14")
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.NewLine = "`n"
try {
    $sp.Open()
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $line = $sp.ReadLine()
            if ($line) { Write-Output $line.TrimEnd("`r") }
        } catch [System.TimeoutException] { }
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
