# Star Trek: Armada II Archipelago integration

The repository contains the Archipelago world, managed mission launcher, and
version-pinned native observer for the stock/GOG Armada II installation.

The game assets remain outside this repository. The release contains two
sidecar helpers, `armada2_observer.dll` and `armada2_injector.exe`, which the
player copies beside `Armada2.exe`; no stock file is replaced or edited.

Build a distributable bundle with:

```powershell
python .\tools\scripts\build_release.py
```

It writes `out\StarTrekArmadaII-AP-0.1.0.zip`, including the `.apworld`, both
native helpers, setup instructions, and a SHA-256 manifest.
