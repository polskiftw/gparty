# gfingerd TODO

1. **Properly handle the malformed GIF geometry case and drain the deferred queue.** Implement tolerant normalization for `GIF frame rectangle is outside its logical canvas`, then enumerate `gfingerd_deferred_gifs`, reprocess each preserved local copy, verify the exact current B2 identity, save the normal gdupe fingerprint, and clear the deferral only after successful settlement.

2. **Scale gdupe down to a consumer of gfingerd fingerprints.** Once gfingerd has been proven reliable against the live library, remove gdupe's foreground fingerprint-generation work. gdupe should retain the shared fingerprint data model/reader plus the preview, duplicate review, survivor selection, exclusion, deletion, and recovery behavior. gfingerd should be the only process responsible for collecting new fingerprints.

3. **Move truly shared native primitives to a neutral library location.** gfingerd currently compiles gdupe's existing database, decoder, and fingerprint source files unchanged so this PR does not modify gdupe. After the behavior split is established, move those shared primitives to a neutral native library in a separate refactor without changing fingerprint outputs.

Deferred GIFs are recoverable technical debt, not discarded media. Until item 1 is complete, the original GIF and JSON recovery note remain under `%LOCALAPPDATA%\GParty\deferred-gifs\`, while the exact-version deferral row lives in `gfingerd_deferred_gifs` inside the normal gdupe SQLite database.
