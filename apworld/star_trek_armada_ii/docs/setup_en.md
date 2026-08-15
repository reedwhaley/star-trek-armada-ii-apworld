# Star Trek: Armada II setup

1. Copy `armada2_observer.dll` and `armada2_injector.exe` beside `Armada2.exe`
   in your Armada II installation. Do not overwrite or modify any stock files.
2. Install `star_trek_armada_ii.apworld` through Archipelago custom worlds and
   restart the Launcher.
3. Start **Star Trek: Armada II Client**. On first launch it asks for the
   Armada II folder; select the folder containing `Armada2.exe`. The selection
   is saved locally, and can be overridden with `--game-root` if the game moves.
4. Connect normally using the standard Archipelago connection controls, then
   choose an unlocked mission from the Mission Launcher tab.

At managed launch, the client temporarily renames only the four application
startup movies (`Activision.bik`, `Intro.bik`, `MadDocSoftware.bik`, and
`Paramount.bik`) to `*.bik.disabled`, injects the version-pinned observer, and
uses the game's stock campaign selector to launch the requested mission. The
four names are restored when that managed game process exits. Mission briefings
and mission movies are never renamed or skipped. On a verified success or
failure it closes only the Armada II process it started. Checks and received
items are persisted before network submission.

In the YAML, `trap_count` accepts `0` through `72`, or `random`. Any omitted
trap slots are replaced one-for-one with randomly selected positive filler
items, preserving the player item-pool size.
