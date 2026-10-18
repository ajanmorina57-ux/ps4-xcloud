# English fork notes

This project is based on Nexo X4 commit `7a7b7b3d51e20486e98e1f757fc0a16856565faf` and remains under its GPL-3.0-only license. See `LICENSE`, `CREDITS.md`, and `THIRD_PARTY_NOTICES.md` for the required attribution and retained notices.

The PSBox home screen, catalog, controller navigation, stream menu, and upstream streaming/authentication implementation are retained. XCloud Stream uses its own name, cloud/play mark, green accent, package title, and English first-run presentation. English is selected for first launch, fallback UI text, public catalog details, store details, and sign-in/session statuses. Other included interface languages remain selectable in Settings.

This fork has not been built or run on PS4. The upstream source checkout does not contain the release dependency archive, generated runtime modules, or the artwork/font/sample assets required by `scripts/empaquetar.sh`. Follow `docs/BUILDING.md` with the matching complete dependency archive and release inputs before producing a package. Upstream test results describe the upstream revision only.
