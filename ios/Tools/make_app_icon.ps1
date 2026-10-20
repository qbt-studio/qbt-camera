# Draws the QBT Camera app icon (1024x1024, opaque) into the asset catalog.
# Run from Windows PowerShell: powershell -File ios/Tools/make_app_icon.ps1
param([string]$Out = "$PSScriptRoot\..\QBTCamera\Assets.xcassets\AppIcon.appiconset\AppIcon.png")
Add-Type -AssemblyName System.Drawing

$size = 1024
$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.PixelOffsetMode = 'HighQuality'
$g.Clear([System.Drawing.Color]::FromArgb(5, 6, 11))

# Brand glows: blue from the top left, violet from the bottom right.
function Glow($cx, $cy, $radius, $color, $alpha) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse($cx - $radius, $cy - $radius, 2 * $radius, 2 * $radius)
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
    $brush.CenterColor = [System.Drawing.Color]::FromArgb($alpha, $color)
    $brush.SurroundColors = @([System.Drawing.Color]::FromArgb(0, $color))
    $g.FillPath($brush, $path)
}
Glow 330 300 720 ([System.Drawing.Color]::FromArgb(43, 127, 255)) 120
Glow 860 900 560 ([System.Drawing.Color]::FromArgb(157, 78, 221)) 70

function RoundedRect($x, $y, $w, $h, $r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# QBT logo from its 100x100 viewBox, centered at 62% of the icon.
$scale = $size * 0.62 / 100
$origin = ($size - 100 * $scale) / 2 + 3 * $scale
function U($v) { return $origin + $v * $scale }
$outline = RoundedRect (U 11) (U 11) (72 * $scale) (72 * $scale) (24 * $scale)
foreach ($halo in @(@(28, 14), @(23, 18), @(19, 24), @(15.5, 34), @(13, 46))) { # soft glow around the stroke
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb($halo[1], 43, 127, 255)), ($halo[0] * $scale)
    $pen.LineJoin = 'Round'
    $g.DrawPath($pen, $outline)
}
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(43, 127, 255)), (10.5 * $scale)
$g.DrawPath($pen, $outline)
$badge = RoundedRect (U 66) (U 66) (28 * $scale) (28 * $scale) (9 * $scale)
$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(83, 155, 255))), $badge)

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out"

