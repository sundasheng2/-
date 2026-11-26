[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('ports', 'connect', 'status', 'read', 'send', 'disconnect')]
    [string]$Command,
    [ValidatePattern('^COM[1-9][0-9]{0,2}$')]
    [string]$Port,
    [ValidateRange(0, 12000000)]
    [int]$Baud,
    [ValidateRange(1, 2147483647)]
    [int]$SessionId,
    [uint64]$Cursor,
    [AllowEmptyString()]
    [string]$Text,
    [string]$Hex,
    [ValidateSet('none', 'cr', 'lf', 'crlf')]
    [string]$Ending = 'crlf'
)

$ErrorActionPreference = 'Stop'
$fields = @($Command.ToUpperInvariant())
if ($Command -eq 'connect') {
    if (!$Port -or [int]$Port.Substring(3) -gt 256) { throw 'Specify -Port COM1 through COM256.' }
    $fields += $Port.Substring(3), $Baud
} elseif ($Command -ne 'ports') {
    if (!$SessionId) { throw 'Specify -SessionId from the connect response.' }
    $fields += $SessionId
}
if ($Command -eq 'read') { $fields += $Cursor }
if ($Command -eq 'send') {
    $hasText = $PSBoundParameters.ContainsKey('Text')
    $hasHex = $PSBoundParameters.ContainsKey('Hex')
    if ($hasText -eq $hasHex) { throw 'Specify either -Text or -Hex.' }
    if ($hasText) {
        $lineEndings = @{ none = ''; cr = "`r"; lf = "`n"; crlf = "`r`n" }
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text + $lineEndings[$Ending])
        $Hex = [BitConverter]::ToString($bytes).Replace('-', '')
    }
    $Hex = $Hex -replace '\s', ''
    if ($Hex -notmatch '\A(?:[0-9a-fA-F]{2})+\z' -or $Hex.Length -gt 1022) {
        throw 'Send requires 1 to 511 bytes.'
    }
    $fields += $Hex
}

$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'stm32-serial-console-ai',
    [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
try {
    $pipe.Connect(2500)
    $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
    $data = [Text.Encoding]::UTF8.GetBytes(($fields -join "`t"))
    $pipe.Write($data, 0, $data.Length)
    $reply = [byte[]]::new(16384)
    $read = $pipe.ReadAsync($reply, 0, $reply.Length)
    if (!$read.Wait(5000)) { throw 'Timed out waiting for the serial assistant. Check the session before retrying a send.' }
    if ($read.Result -eq 0 -or !$pipe.IsMessageComplete) { throw 'Incomplete reply from the serial assistant.' }
    $response = [Text.Encoding]::UTF8.GetString($reply, 0, $read.Result)
    if ($response.StartsWith('ERR ')) { throw $response.TrimEnd() }
    if (!$response.StartsWith("OK`n") -and !$response.StartsWith('OK ')) { throw 'Invalid reply.' }
    $response
} finally {
    $pipe.Dispose()
}
