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

## `circle-tcp-robust.patch`

Applied after `circle-wifi-149.patch` (marker `mmbasic-tcp-robust` in
`lib/net/tcpconnection.cpp`). TCP receive path hardening for TERM:

- `CTCPConnection` step 7: a segment that starts before `RCV.NXT` but
  extends past it (a coalesced retransmission after our ACK was lost) is
  trimmed and its unseen tail accepted, instead of being discarded whole.
  A pure duplicate is re-ACKed.
- `CReassemblyQueue` no longer disables itself on overlap. Enqueue drops
  fully covered segments (either direction); Dequeue trims partial
  overlaps and discards stale entries instead of matching `SEQ` for
  equality only.
- `CTCPConnection::Receive()` drains `m_RxQueue` before reporting
  `m_nErrno`, and an orderly close (peer FIN, local close) returns
  `-NET_ERROR_NOT_CONNECTED` rather than `-NET_ERROR_CONNECTION_RESET`,
  so the caller can name the close reason.

Regenerate by editing the (already patched) submodule tree and diffing
against a pristine `git archive` copy with `circle-wifi-149.patch`
applied; `tests/test_circle_patches.py` checks both patches apply cleanly
in that order and exercises the reassembly queue on the host.

Do not commit a dirty Circle submodule; the parent tree only vendors the
patch files.
