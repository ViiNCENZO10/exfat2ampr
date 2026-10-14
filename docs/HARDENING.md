# Hardening of 14 October 2026

Reliability work integrated in version 1.95, first prepared by an outside review (Codex) and then adjusted:
directory syncs are best effort (some PS5 storages cannot sync a folder), and a loose file is hashed while it
is copied instead of being read again from the source and the copy.

- Refuse reconverting a folder containing an AMPR manifest, including forced conversion.
- Fail an incomplete inventory, unsupported file types and source symbolic links.
- Hash loose files with SHA-256 and persist a manifest-bound `.loose` sidecar.
- Resume publication after volume/manifest renames; persist the next stage before removing checkpoints.
- Validate manifest paths, checksums and layouts for repair as well as verification.
- Bound checkpoint lengths/indices and checksum new checkpoint headers (version 3).
- Reserve maintenance paths against queued/running conversions; protect paused sources.
- Replace shared volatile flags with atomics; protect pairing, estimates and pause reasons.
- Reuse compression threads and fix reduced-worker chunk coverage.
- Strict JSON grammar, bounded number parsing, Unicode pairs and update manifest validation.
- Unique temporary files, parent-directory synchronization, explicit I/O failures and safer recursive deletion.

## Compatibility and limitations

The emulator's AMPR v4 format is unchanged. The extra `ampr_assets.index.loose` is for conversion
verification. Keep it alongside the converted folder. An old paused job without it returns to the
copy phase while its source is available. Standalone full verification of an old completed folder
without that sidecar fails; repair of the emulator index does not require it.

New checkpoints are version 3; older version-2 checkpoints remain bounded on reading, but do not gain
checksums retroactively. Header checksums are not authentication and do not cover every saved buffer.
Verification restarts from the beginning after a process restart to detect modifications during a pause.
Loose hashing adds one read of each loose copy during the final verification. Worker reuse reduces thread creation; no measured PS5 speedup is claimed.

No PS5 hardware, exFAT power-cut qualification, reference-emulator integration or UI screenshot suite
has been executed for these local changes. Production builds and those tests remain required.
A modification of source data with identical size/mtime during packing is not fully excluded.
Protection against concurrent external filesystem mutation is not a general sandbox.
