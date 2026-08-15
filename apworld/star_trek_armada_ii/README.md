# Armada II APWorld metadata baseline

This package contains 302 locations: 155 campaign checks plus 147 deduplicated,
mission-tech-gated first-build locations. It includes campaign access items,
faction keys, Fluidic Space, Victory, the durable received-item/check ledger,
and the version-pinned observer protocol.

The launcher tab enforces mission access before it sends an unlocked campaign
request to the observer. The native adapter resolves the requested stock BZN
from the selector's own filename table, confirms the matching selector row on
the Armada II UI thread, and lets the stock `SetupMission` continuation launch
it. It never uses a direct map loader.

## Native helper installation

Copy `armada2_observer.dll` and `armada2_injector.exe` into the installed
Armada II folder, beside `Armada2.exe`. The client asks for that folder on its
first GUI launch and stores the selection locally. It starts only that copy of
Armada II after temporarily renaming only its four application-startup Binks,
injects the observer, and launches unlocked missions
through the stock campaign selector. Mission briefings and mission movies are
not skipped. On a verified success or failure, it closes only the process that
it started.

The world has a fixed 268-item permanent/filler/trap allocation. YAML
`trap_count` accepts `0` through `72` or `random`; omitted traps are replaced
one-for-one by randomized positive filler while keeping the pool size fixed.

Regenerate the packaged catalog after objective-taxonomy changes:

```powershell
python .\tools\scripts\build_objective_catalog.py `
  --objectives 'E:\GOG\Star Trek Armada II\objectives' `
  --output .\apworld\star_trek_armada_ii\data\campaign-objectives.json `
  --summary .\apworld\star_trek_armada_ii\data\campaign-objectives.md
```
