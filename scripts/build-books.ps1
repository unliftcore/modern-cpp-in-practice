Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location $repoRoot
try {
    $outputDirs = @(
        (Join-Path $repoRoot "docs/en"),
        (Join-Path $repoRoot "docs/zh-CN")
    )

    foreach ($dir in $outputDirs) {
        if (Test-Path $dir) {
            Remove-Item $dir -Recurse -Force
        }
    }

    mdbook build
    mdbook build translations/zh-CN
}
finally {
    Pop-Location
}