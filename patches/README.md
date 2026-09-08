# Circle patches

Circle stays a git submodule (`circle/`). These diffs are applied by
`scripts/build.sh` (and therefore by `package-release.sh`) when the
marker comment is not already present.

## `circle-wifi-149.patch`

Issue #149 (stale IPCONFIG / Pi 400 keepalive / TERM second connect):

- `ether4330.c` `wlinit()`: program firmware `mkeep_alive` for chip
  `0x4345` (43455 and 43456) as well as `0x4330`. 802.11 power save stays
  off on 4345.
- `CTCPConnection::Close()` wakes a blocking `Connect()` when aborting
  `SYN-SENT`, so TERM/CONNECT can cancel a ~90s SYN wait.
- `CTransportLayer::AbortConnecting()` closes in-flight SYN-SENT TCBs.

Do not commit a dirty Circle submodule; the parent tree only vendors the
patch file.
