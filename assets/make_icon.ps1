# Draws the pack's notification icon: assets/meowy.ico (16-256 px, the
# taskbar badge, the tray icon and the Start-menu shortcut) and
# assets/notify.png (the toast's logo). Both are committed; run this again
# only to change the drawing:   powershell -File assets/make_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Draw([int]$size) {
	$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
	$g.Clear([System.Drawing.Color]::Transparent)
	$s = $size / 64.0

	# Rounded square in the loading screen's rose.
	$r = 14 * $s
	$path = New-Object System.Drawing.Drawing2D.GraphicsPath
	$path.AddArc(0, 0, 2 * $r, 2 * $r, 180, 90)
	$path.AddArc($size - 2 * $r - 1, 0, 2 * $r, 2 * $r, 270, 90)
	$path.AddArc($size - 2 * $r - 1, $size - 2 * $r - 1, 2 * $r, 2 * $r, 0, 90)
	$path.AddArc(0, $size - 2 * $r - 1, 2 * $r, 2 * $r, 90, 90)
	$path.CloseFigure()
	$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 225, 29, 72))), $path)

	# A white cat head: two ears and a round face.
	$white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
	$ear1 = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF (13 * $s), (14 * $s)), (New-Object System.Drawing.PointF (28 * $s), (24 * $s)), (New-Object System.Drawing.PointF (15 * $s), (34 * $s)))
	$ear2 = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF (51 * $s), (14 * $s)), (New-Object System.Drawing.PointF (36 * $s), (24 * $s)), (New-Object System.Drawing.PointF (49 * $s), (34 * $s)))
	$g.FillPolygon($white, $ear1)
	$g.FillPolygon($white, $ear2)
	$g.FillEllipse($white, 12 * $s, 20 * $s, 40 * $s, 34 * $s)

	# Eyes (left out at 16 px, where they'd only be noise).
	if ($size -ge 24) {
		$dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 22, 21, 27))
		$g.FillEllipse($dark, 21 * $s, 32 * $s, 6 * $s, 7 * $s)
		$g.FillEllipse($dark, 37 * $s, 32 * $s, 6 * $s, 7 * $s)
	}
	$g.Dispose()
	return $bmp
}

function PngBytes([int]$size) {
	$bmp = Draw $size
	$ms = New-Object System.IO.MemoryStream
	$bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
	$bmp.Dispose()
	return ,$ms.ToArray()
}

$here = $PSScriptRoot
[System.IO.File]::WriteAllBytes((Join-Path $here 'notify.png'), (PngBytes 128))

# .ico with PNG-compressed entries (Windows Vista and later read these at every size).
$sizes = 16, 24, 32, 48, 64, 256
$images = @()
foreach ($sz in $sizes) { $images += ,(PngBytes $sz) }
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
	$dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
	$w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
	$w.Write([UInt16]1); $w.Write([UInt16]32)
	$w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
	$offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $here 'meowy.ico'), $ms.ToArray())
Get-ChildItem $here -Include *.ico, *.png -Recurse | Select-Object Name, Length
