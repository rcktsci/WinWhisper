<#
    Generates .ico files for WinWhisper with pure math (no GDI / no System.Drawing),
    so the resource pipeline stays reproducible on any machine.

    Glyph: a microphone (capsule + cradle + stand + base), 4x4 supersampled.

    Usage:
        pwsh -File tools\make-icons.ps1
#>

param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\src\res')
)

$ErrorActionPreference = 'Stop'

function Test-MicGlyph {
    param([double]$X, [double]$Y)

    # 1. Capsule body: stadium, x in [0.35,0.65], y in [0.13,0.55]
    $hw = 0.15; $hh = 0.21; $r = 0.15; $cy = 0.34
    $qx = [Math]::Abs($X - 0.5) - ($hw - $r)
    $qy = [Math]::Abs($Y - $cy) - ($hh - $r)
    $d = [Math]::Sqrt([Math]::Max($qx, 0) * [Math]::Max($qx, 0) + [Math]::Max($qy, 0) * [Math]::Max($qy, 0))
    $d += [Math]::Min([Math]::Max($qx, $qy), 0.0) - $r
    if ($d -le 0) { return $true }

    # 2. Cradle arc: bottom half of an annulus centred at (0.5, 0.46)
    $dx = $X - 0.5; $dy = $Y - 0.46
    $dist = [Math]::Sqrt($dx * $dx + $dy * $dy)
    if ($dist -ge 0.225 -and $dist -le 0.30) {
        $ang = [Math]::Atan2($dy, $dx) * 180.0 / [Math]::PI   # 0 = right, 90 = down
        if ($ang -ge 12 -and $ang -le 168) { return $true }
    }

    # 3. Stand
    if ([Math]::Abs($X - 0.5) -le 0.035 -and $Y -ge 0.70 -and $Y -le 0.855) { return $true }

    # 4. Base
    if ([Math]::Abs($X - 0.5) -le 0.155 -and $Y -ge 0.85 -and $Y -le 0.905) { return $true }

    return $false
}

function New-GlyphBitmap {
    param([int]$Size, [int]$R, [int]$G, [int]$B)

    $px = New-Object 'int[]' ($Size * $Size)   # alpha
    $ss = 4
    for ($py = 0; $py -lt $Size; $py++) {
        for ($pxx = 0; $pxx -lt $Size; $pxx++) {
            $hits = 0
            for ($sy = 0; $sy -lt $ss; $sy++) {
                for ($sx = 0; $sx -lt $ss; $sx++) {
                    $nx = ($pxx + ($sx + 0.5) / $ss) / $Size
                    $ny = ($py + ($sy + 0.5) / $ss) / $Size
                    if (Test-MicGlyph -X $nx -Y $ny) { $hits++ }
                }
            }
            $px[$py * $Size + $pxx] = [int](255.0 * $hits / ($ss * $ss))
        }
    }

    # Pack as bottom-up BGRA rows plus the 1bpp AND mask.
    $rowBytes = $Size * 4
    $maskStride = [int]([Math]::Floor(($Size + 31) / 32.0)) * 4
    $bytes = New-Object byte[] ($rowBytes * $Size + $maskStride * $Size)
    $i = 0
    for ($py = $Size - 1; $py -ge 0; $py--) {
        for ($pxx = 0; $pxx -lt $Size; $pxx++) {
            $a = $px[$py * $Size + $pxx]
            $bytes[$i++] = [byte]$B
            $bytes[$i++] = [byte]$G
            $bytes[$i++] = [byte]$R
            $bytes[$i++] = [byte]$a
        }
    }
    for ($py = 0; $py -lt $Size; $py++) {
        $offset = $rowBytes * $Size + $maskStride * $py
        for ($pxx = 0; $pxx -lt $Size; $pxx += 8) {
            $bits = 0
            for ($b = 0; $b -lt 8; $b++) {
                $x = $pxx + $b
                if ($x -lt $Size -and $px[$py * $Size + $x] -lt 128) { $bits += (1 -shl $b) }
            }
            $bytes[$offset + [int]($pxx / 8)] = [byte]$bits
        }
    }
    # Prepend BITMAPINFOHEADER (height is doubled: colour rows + AND mask).
    $hdr = New-Object byte[] 40
    [Array]::Copy([BitConverter]::GetBytes([UInt32]40), 0, $hdr, 0, 4)
    [Array]::Copy([BitConverter]::GetBytes([Int32]$Size), 0, $hdr, 4, 4)
    [Array]::Copy([BitConverter]::GetBytes([Int32]($Size * 2)), 0, $hdr, 8, 4)
    [Array]::Copy([BitConverter]::GetBytes([UInt16]1), 0, $hdr, 12, 2)
    [Array]::Copy([BitConverter]::GetBytes([UInt16]32), 0, $hdr, 14, 2)
    [Array]::Copy([BitConverter]::GetBytes([UInt32]($rowBytes * $Size + $maskStride * $Size)), 0, $hdr, 20, 4)

    $dib = New-Object byte[] ($hdr.Length + $bytes.Length)
    [Array]::Copy($hdr, 0, $dib, 0, $hdr.Length)
    [Array]::Copy($bytes, 0, $dib, $hdr.Length, $bytes.Length)
    return , $dib
}

function New-IcoFile {
    param([int[]]$Sizes, [int]$R, [int]$G, [int]$B, [string]$Path)

    $images = @()
    foreach ($s in $Sizes) { $images += , (New-GlyphBitmap -Size $s -R $R -G $G -B $B) }

    $out = New-Object System.Collections.Generic.List[byte]
    $out.AddRange([byte[]](0, 0, 1, 0))                 # reserved, type = icon
    $out.AddRange([byte[]]@([BitConverter]::GetBytes([UInt16]$Sizes.Count)))

    $offset = 6 + 16 * $Sizes.Count
    for ($i = 0; $i -lt $Sizes.Count; $i++) {
        $s = $Sizes[$i]
        $entry = New-Object byte[] 16
        $entry[0] = [byte]$(if ($s -ge 256) { 0 } else { $s })
        $entry[1] = [byte]$(if ($s -ge 256) { 0 } else { $s })
        $entry[2] = 0; $entry[3] = 0
        [Array]::Copy([BitConverter]::GetBytes([UInt16]1), 0, $entry, 4, 2)
        [Array]::Copy([BitConverter]::GetBytes([UInt16]32), 0, $entry, 6, 2)
        [Array]::Copy([BitConverter]::GetBytes([UInt32]$images[$i].Length), 0, $entry, 8, 4)
        [Array]::Copy([BitConverter]::GetBytes([UInt32]$offset), 0, $entry, 12, 4)
        $out.AddRange($entry)
        $offset += $images[$i].Length
    }

    for ($i = 0; $i -lt $images.Count; $i++) {
        $out.AddRange($images[$i])
    }

    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
    [System.IO.File]::WriteAllBytes($Path, $out.ToArray())
    Write-Host ("{0}: {1} sizes, {2} bytes" -f (Split-Path $Path -Leaf), ($Sizes -join '/'), $out.Count)
}

$sizes = @(16, 24, 32, 48, 64, 128)
New-IcoFile -Sizes $sizes -R 0x3A -G 0x3A -B 0x3C -Path (Join-Path $OutDir 'app.ico')
New-IcoFile -Sizes $sizes -R 0x1F -G 0x7A -B 0xE8 -Path (Join-Path $OutDir 'app-active.ico')