#requires -Version 5.1
# Generates the application icon: resources/app.ico (exe icon) and resources/icons/app_<size>.png (Qt window/tray icon).
# Each size is drawn natively instead of downscaled so small sizes stay sharp.
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$resourceDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'resources'
$pngDir = Join-Path $resourceDir 'icons'
New-Item -ItemType Directory -Force -Path $pngDir | Out-Null

function New-RoundedRectPath {
    param([float]$X, [float]$Y, [float]$Width, [float]$Height, [float]$Radius)
    $diameter = $Radius * 2
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng {
    param([int]$Size)

    # Design coordinates are on a 256 grid.
    $scale = $Size / 256.0
    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.Clear([System.Drawing.Color]::Transparent)

        # Background: rounded square with a blue diagonal gradient.
        # Whole-pixel inset keeps the square's edges crisp at small sizes.
        $inset = [Math]::Round(8 * $scale)
        $extent = $Size - 2 * $inset
        $background = New-RoundedRectPath $inset $inset $extent $extent (52 * $scale)
        $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            (New-Object System.Drawing.PointF(0, 0)),
            (New-Object System.Drawing.PointF($Size, $Size)),
            [System.Drawing.Color]::FromArgb(255, 56, 152, 255),
            [System.Drawing.Color]::FromArgb(255, 22, 74, 170))
        $graphics.FillPath($gradient, $background)
        $gradient.Dispose()
        $background.Dispose()

        # Foreground: two clockwise white arcs with arrowheads (sync symbol).
        # Thicker strokes at small sizes keep the symbol readable at 16px.
        $stroke = [Math]::Max(24 * $scale, 1.9)
        $radius = 66 * $scale
        $center = $Size / 2.0
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $stroke)
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $pen.CustomEndCap = New-Object System.Drawing.Drawing2D.AdjustableArrowCap(2.1, 1.7, $true)
        $box = New-Object System.Drawing.RectangleF(($center - $radius), ($center - $radius), (2 * $radius), (2 * $radius))
        $graphics.DrawArc($pen, $box, 200, 115)
        $graphics.DrawArc($pen, $box, 20, 115)
        $pen.Dispose()

        $stream = New-Object System.IO.MemoryStream
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        return $stream.ToArray()
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

$images = @()
foreach ($size in $sizes) {
    $bytes = New-IconPng $size
    [System.IO.File]::WriteAllBytes((Join-Path $pngDir "app_$size.png"), $bytes)
    $images += , @($size, $bytes)
}

# ICO container with PNG-compressed entries (supported since Windows Vista).
$icoPath = Join-Path $resourceDir 'app.ico'
$file = [System.IO.File]::Create($icoPath)
$writer = New-Object System.IO.BinaryWriter($file)
try {
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($image in $images) {
        $dimension = if ($image[0] -ge 256) { 0 } else { $image[0] }
        $writer.Write([Byte]$dimension)
        $writer.Write([Byte]$dimension)
        $writer.Write([Byte]0)
        $writer.Write([Byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$image[1].Length)
        $writer.Write([UInt32]$offset)
        $offset += $image[1].Length
    }
    foreach ($image in $images) {
        $writer.Write([Byte[]]$image[1])
    }
}
finally {
    $writer.Dispose()
}

Write-Host "Icon written: $icoPath"
Write-Host "PNG icons written: $pngDir"
