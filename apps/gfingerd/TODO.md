# gfingerd TODO

1. **Properly fix malformed GIF geometry handling.** Port/finish the tolerant GIF normalization work proven by the `giftest` sweep, then enumerate `deferred_gifs` in the fingerprint registry and reprocess every saved local copy. Only clear a deferral after the exact B2 object identity is verified and its fingerprint is successfully persisted.

Deferred GIFs are intentionally recoverable technical debt, not discarded media. Until item 1 is implemented, `gfingerd` stores the original downloaded GIF under the local `deferred-gifs` directory, writes a JSON recovery note beside it, records the exact B2 file ID in `fingerprints.sqlite3`, excludes that exact version from normal pending work, and reports it separately in the GUI.
