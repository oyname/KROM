# =============================================================================
# KROM Engine - tools/shader_cross/compare_images.ps1
# Compares two PNGs of equal size and prints how many pixels differ and the
# largest channel difference.
# Example: compare_images.ps1 hand.png generated.png
# =============================================================================
param([string]$PathA, [string]$PathB)
Add-Type -AssemblyName System.Drawing
$imgA = [System.Drawing.Bitmap]::FromFile($PathA)
$imgB = [System.Drawing.Bitmap]::FromFile($PathB)
if ($imgA.Width -ne $imgB.Width -or $imgA.Height -ne $imgB.Height) { "size differs: $($imgA.Width)x$($imgA.Height) vs $($imgB.Width)x$($imgB.Height)"; exit 1 }
$rect = New-Object System.Drawing.Rectangle 0, 0, $imgA.Width, $imgA.Height
$fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$da = $imgA.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
$db = $imgB.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
$n = $da.Stride * $imgA.Height
$ba = [byte[]]::new($n); $bb = [byte[]]::new($n)
[System.Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $n)
[System.Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $n)
$imgA.UnlockBits($da); $imgB.UnlockBits($db)
$diffPixels = 0; $maxDiff = 0; $nonBlack = 0
for ($i = 0; $i -lt $n; $i += 4) {
    $d = [Math]::Max([Math]::Max([Math]::Abs($ba[$i] - $bb[$i]), [Math]::Abs($ba[$i+1] - $bb[$i+1])), [Math]::Abs($ba[$i+2] - $bb[$i+2]))
    if ($d -gt 0) { $diffPixels++ }
    if ($d -gt $maxDiff) { $maxDiff = $d }
    if (($ba[$i] + $ba[$i+1] + $ba[$i+2]) -gt 30) { $nonBlack++ }
}
"pixels=$($imgA.Width * $imgA.Height) differing=$diffPixels maxChannelDiff=$maxDiff nonBlackInA=$nonBlack"
