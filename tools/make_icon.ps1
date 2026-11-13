# Generates src/wxrvid.ico with 16/24/32/48/64/128 BMP entries and a 256 PNG entry.
# The notification area needs real 16/24/32 pixel images; a 256-only PNG icon shows blank
# under Wine.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root "src\wxrvid.ico"

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $margin = [Math]::Max(1.0, $size * 0.06)
    $diameter = $size - 2.0 * $margin
    $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 216, 32, 32))
    $graphics.FillEllipse($brush, [single]$margin, [single]$margin, [single]$diameter, [single]$diameter)

    $penWidth = [Math]::Max(1.0, $size * 0.05)
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(180, 110, 0, 0), [single]$penWidth)
    $graphics.DrawEllipse($pen, [single]$margin, [single]$margin, [single]$diameter, [single]$diameter)

    if ($size -ge 24) {
        $highlight = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(110, 255, 255, 255))
        $graphics.FillEllipse($highlight, [single]($margin + $size * 0.20), [single]($margin + $size * 0.14),
                              [single]($size * 0.28), [single]($size * 0.22))
        $highlight.Dispose()
    }

    $pen.Dispose()
    $brush.Dispose()
    $graphics.Dispose()
    return $bmp
}

function Get-BmpImage([System.Drawing.Bitmap]$bmp, [int]$size) {
    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($stream)
    $writer.Write([UInt32]40)
    $writer.Write([Int32]$size)
    $writer.Write([Int32]($size * 2))
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]($size * $size * 4))
    $writer.Write([Int32]0)
    $writer.Write([Int32]0)
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]0)

    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $pixel = $bmp.GetPixel($x, $y)
            $writer.Write($pixel.B)
            $writer.Write($pixel.G)
            $writer.Write($pixel.R)
            $writer.Write($pixel.A)
        }
    }

    $maskRow = [int]([Math]::Ceiling($size / 32.0) * 4)
    $zeros = New-Object byte[] $maskRow
    for ($y = 0; $y -lt $size; $y++) {
        $writer.Write($zeros)
    }

    $writer.Flush()
    return $stream.ToArray()
}

function Get-PngImage([System.Drawing.Bitmap]$bmp) {
    $stream = New-Object System.IO.MemoryStream
    $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    return $stream.ToArray()
}

$images = @()
foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size
    if ($size -ge 256) {
        $data = Get-PngImage $bmp
    } else {
        $data = Get-BmpImage $bmp $size
    }
    $images += [pscustomobject]@{ Size = $size; Data = $data }
    $bmp.Dispose()
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([UInt16]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]$images.Count)

$offset = 6 + 16 * $images.Count
foreach ($image in $images) {
    $dimension = if ($image.Size -ge 256) { 0 } else { $image.Size }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]$image.Data.Length)
    $writer.Write([UInt32]$offset)
    $offset += $image.Data.Length
}
foreach ($image in $images) {
    $writer.Write([byte[]]$image.Data)
}

$writer.Flush()
[System.IO.File]::WriteAllBytes($output, $stream.ToArray())
Write-Host "wrote $output ($($stream.Length) bytes)"
