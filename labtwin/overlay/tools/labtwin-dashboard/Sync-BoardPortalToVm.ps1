[CmdletBinding()]
param(
    [string]$VmHost = "openvela-vm",
    [string]$SshKey = "$env:USERPROFILE\.ssh\id_ed25519",
    [string]$RemoteTarget = "/home/li/openvela/vendor/allwinnertech/lichee/board/common/data/res/labtwin-admin"
)

$ErrorActionPreference = "Stop"
$portalRoot = Split-Path -Parent $PSCommandPath
$dist = Join-Path $portalRoot "dist-board"
$remoteTarget = $RemoteTarget
if ($remoteTarget -notmatch '^/home/li/(openvela/vendor/allwinnertech|openvela-worktrees/[a-z0-9-]+-vendor)/lichee/board/common/data/res/labtwin-admin$') {
    throw "Portal target must be an explicit vendor ROMFS input directory."
}
$remoteStage = (& ssh -i $SshKey -o BatchMode=yes $VmHost 'mktemp -d /tmp/labtwin-board-portal-sync.XXXXXX').Trim()
if ($LASTEXITCODE -ne 0 -or $remoteStage -notmatch '^/tmp/labtwin-board-portal-sync\.[A-Za-z0-9]+$') {
    throw "Could not create an isolated VM staging directory."
}

if (-not (Test-Path -LiteralPath $SshKey)) {
    throw "SSH key was not found: $SshKey"
}

Push-Location $portalRoot
try {
    npm run build:board
} finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) {
    throw "Board portal build failed; firmware resources were not changed."
}

# Vite also copies public JS/CSS/HTML verbatim. Keep the ROMFS byte stream
# stable across Windows and Linux before computing its source manifest.
Get-ChildItem -LiteralPath $dist -Recurse -File |
    Where-Object { $_.Extension -in @('.html', '.css', '.js') } |
    ForEach-Object {
        $value = [IO.File]::ReadAllText($_.FullName)
        if ($value.Contains("`r")) {
            $value = $value.Replace("`r`r`n", "`n").Replace("`r`n", "`n")
            [IO.File]::WriteAllText($_.FullName, $value, [Text.UTF8Encoding]::new($false))
        }
    }

$index = Get-Content -Raw (Join-Path $dist "index.html")
if ($index -notmatch '/assets/labtwin-logo\.png' -or
    $index -notmatch 'voice-config-portal\.js' -or
    $index -notmatch 'recorder-portal\.js') {
    throw "Board portal output is missing the asset-logo or required extension references."
}
$versionedAssets = [regex]::Matches($index, '(?:src|href)="/assets/([A-Za-z0-9._-]+)\?v=([a-f0-9]{16})"')
if ($versionedAssets.Count -ne 5) {
    throw "Board portal must content-version all four maintained extensions and the favicon."
}
foreach ($asset in $versionedAssets) {
    $assetFile = Join-Path (Join-Path $dist 'assets') $asset.Groups[1].Value
    $actualVersion = (Get-FileHash -LiteralPath $assetFile -Algorithm SHA256).Hash.ToLowerInvariant().Substring(0, 16)
    if ($actualVersion -ne $asset.Groups[2].Value) {
        throw "A public resource version does not match its actual ROMFS bytes."
    }
}
$bundleMatch = [regex]::Match($index, 'src="/assets/(index-[^"]+\.js)"')
if (-not $bundleMatch.Success) {
    throw "Board portal output has no main JavaScript bundle reference."
}
$bundle = $bundleMatch.Groups[1].Value
$sourceRevision = (& git -C $portalRoot rev-parse HEAD).Trim()
if (& git -C $portalRoot status --porcelain) { throw "Commit reviewed Portal source changes before ROMFS synchronization." }
$remoteRevision = (& ssh -i $SshKey -o BatchMode=yes $VmHost 'git -C /home/li/openvela/tools/labtwin-dashboard rev-parse HEAD').Trim()
if ($LASTEXITCODE -ne 0 -or $sourceRevision -ne $remoteRevision) {
    throw "Windows and Ubuntu maintained Portal source revisions differ; transfer source before ROMFS sync."
}
$manifest = Get-ChildItem -LiteralPath $dist -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($dist.Length + 1).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
}
[IO.File]::WriteAllText((Join-Path $dist 'RESOURCE_SHA256SUMS'), ($manifest -join "`n") + "`n", [Text.UTF8Encoding]::new($false))

& ssh -i $SshKey -o BatchMode=yes $VmHost "mkdir -p $remoteTarget"
if ($LASTEXITCODE -ne 0) { throw "Could not prepare the VM portal staging directory." }

& scp -i $SshKey -o BatchMode=yes -r "$dist\*" "${VmHost}:$remoteStage/"
if ($LASTEXITCODE -ne 0) { throw "Could not transfer the board portal build to the VM." }

$remoteVerify = @(
    "set -eu",
    "cd $remoteStage",
    "sha256sum -c RESOURCE_SHA256SUMS >/dev/null",
    # This dedicated directory is versioned ROMFS input. Remove only obsolete
    # generated hash bundles, never maintained extensions or unrelated files.
    "cd $remoteTarget",
    "for candidate in assets/index-*.js assets/index-*.css assets/report-export-*.js assets/charts-*.js assets/rolldown-runtime-*.js; do",
    "  test -f `"`$candidate`" || continue",
    "  test -f $remoteStage/`"`$candidate`" || rm -- `"`$candidate`"",
    "done",
    "cp -a $remoteStage/. $remoteTarget/",
    "grep -Fq '/assets/labtwin-logo.png' $remoteTarget/index.html",
    "grep -Fq 'voice-config-portal.js' $remoteTarget/index.html",
    "grep -Fq 'recorder-portal.js' $remoteTarget/index.html",
    "test -f $remoteTarget/assets/$bundle",
    "grep -Fq '/assets/labtwin-logo.png' $remoteTarget/assets/$bundle",
    "cd $remoteTarget",
    "sha256sum -c RESOURCE_SHA256SUMS >/dev/null",
    "printf '%s\n' '$sourceRevision' > SOURCE_REVISION",
    "rm -r -- $remoteStage",
    "echo LABTWIN_BOARD_PORTAL_SYNC=PASS"
) -join "`n"

& ssh -i $SshKey -o BatchMode=yes $VmHost $remoteVerify
if ($LASTEXITCODE -ne 0) {
    throw "Portal sync verification failed. Do not package this firmware."
}
