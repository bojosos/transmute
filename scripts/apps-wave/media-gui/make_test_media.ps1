# Writes small, deterministic test media for the apps wave (no external tools):
#   tone-440.wav   2 s of a 440 Hz sine, 48 kHz, 16-bit PCM stereo (left 440 Hz, right 660 Hz)
#   bars.y4m       320x240 YUV4MPEG2 4:2:0, 25 fps, 50 frames: eight vertical luma bars, with a white
#                  square that moves 4 px right per frame (so a decoder's output can be checked per frame)
# Usage: make_test_media.ps1 -OutDir <dir>
param([Parameter(Mandatory)][string]$OutDir)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null

# --- WAV ---
$rate = 48000; $seconds = 2; $channels = 2
$frames = $rate * $seconds
$data = New-Object byte[] ($frames * $channels * 2)
for ($i = 0; $i -lt $frames; $i++) {
    $l = [int][math]::Round(12000 * [math]::Sin(2 * [math]::PI * 440 * $i / $rate))
    $r = [int][math]::Round(12000 * [math]::Sin(2 * [math]::PI * 660 * $i / $rate))
    $o = $i * 4
    $data[$o] = $l -band 0xff; $data[$o + 1] = ($l -shr 8) -band 0xff
    $data[$o + 2] = $r -band 0xff; $data[$o + 3] = ($r -shr 8) -band 0xff
}
$stream = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($stream)
$w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([uint32](36 + $data.Length))
$w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $w.Write([uint32]16); $w.Write([uint16]1); $w.Write([uint16]$channels)
$w.Write([uint32]$rate); $w.Write([uint32]($rate * $channels * 2)); $w.Write([uint16]($channels * 2)); $w.Write([uint16]16)
$w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([uint32]$data.Length); $w.Write($data)
$w.Flush()
[IO.File]::WriteAllBytes((Join-Path $OutDir 'tone-440.wav'), $stream.ToArray())

# --- Y4M ---
$width = 320; $height = 240; $count = 50
$out = New-Object IO.MemoryStream
$header = [Text.Encoding]::ASCII.GetBytes("YUV4MPEG2 W$width H$height F25:1 Ip A1:1 C420jpeg`n")
$out.Write($header, 0, $header.Length)
$frameTag = [Text.Encoding]::ASCII.GetBytes("FRAME`n")
$bars = [byte[]](235, 210, 180, 150, 120, 90, 60, 16)
$row = New-Object byte[] $width
for ($x = 0; $x -lt $width; $x++) { $row[$x] = $bars[[math]::Floor($x * 8 / $width)] }
$chroma = New-Object byte[] (($width / 2) * ($height / 2))
for ($i = 0; $i -lt $chroma.Length; $i++) { $chroma[$i] = 128 }
for ($f = 0; $f -lt $count; $f++) {
    $out.Write($frameTag, 0, $frameTag.Length)
    $luma = New-Object byte[] ($width * $height)
    for ($y = 0; $y -lt $height; $y++) { [Array]::Copy($row, 0, $luma, $y * $width, $width) }
    $sx = 40 + 4 * $f
    for ($y = 100; $y -lt 140; $y++) { for ($x = $sx; $x -lt $sx + 40; $x++) { $luma[$y * $width + $x] = 255 } }
    $out.Write($luma, 0, $luma.Length)
    $out.Write($chroma, 0, $chroma.Length); $out.Write($chroma, 0, $chroma.Length)
}
[IO.File]::WriteAllBytes((Join-Path $OutDir 'bars.y4m'), $out.ToArray())
Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
