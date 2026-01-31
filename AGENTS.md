# AGENTS.md

This is a Swift wrapper around a battle-tested C package known as hfsutils which is used for opening, reading, and modifying HFS volumes/disk images. The wrapper should make minimal changes to the C code. It should make it easier to use in Swift.

## HFSKit repo expectations

- This is a Swift Package.
- After modifying code in `Sources/` or `Tests/`, run `swift test` from the package root.
- Treat failing `swift test` as a hard error: fix the tests before doing any refactors or extra changes.
