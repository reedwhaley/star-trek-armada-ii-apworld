param(
    [string]$ResearchRoot = 'E:\GOG\Star Trek Armada II\armada2-research',
    [string]$GameRoot = 'E:\GOG\Star Trek Armada II'
)

$injector = Join-Path $ResearchRoot 'build-injector\Release\armada2_injector.exe'
$observer = Join-Path $ResearchRoot 'build-observer\Release\armada2_observer.dll'
$game = Join-Path $GameRoot 'Armada2.exe'
foreach ($path in $injector, $observer, $game) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file is missing: $path" }
}
if (Get-Process Armada2 -ErrorAction SilentlyContinue) { throw 'Armada2.exe is already running.' }
& $injector --launch $game $observer
if ($LASTEXITCODE -ne 0) { throw "Armada II suspended launch/injection failed: $LASTEXITCODE" }
