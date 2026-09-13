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
applied; `tests/test_circle_patches.py` checks the patches apply cleanly
in that order and exercises the reassembly queue on the host.

## `circle-tcp-send.patch`

Applied after `circle-tcp-robust.patch` (marker `mmbasic-tcp-send` in
`lib/net/netdevlayer.cpp`). TCP send path must not open a sequence hole:

- `CNetDeviceLayer::Process()`: if `SendFrame()` fails, `EnqueueFront`
  the buffer instead of deleting it. A later success must not leave the
  NIC while an earlier frame was dropped.
- `CNetBufferQueue::EnqueueFront()` restores that buffer at the head.
- `CTCPConnection::SendNewSegment()`: if `SendSegment()` returns FALSE,
  leave `SND.NXT` and the TxQueue peek unchanged.

## `circle-tcp-ack.patch`

Applied after `circle-tcp-send.patch` (marker `mmbasic-tcp-ack` in
`lib/net/tcpconnection.cpp`). Telnet-sized traffic:

- ESTABLISHED 1-byte ACKs flush the matching TxQueue entry (upstream
  skipped `nBytesAck == 1`, so every keystroke stayed queued and
  retransmit replayed stale payload at `SND.UNA`).
- `CNetBufferQueue::Flush()` trims a partial ACK from the head buffer.
- `RCV.WND` tracks unread `m_RxQueue` **and** reassembly-queue bytes, so
  out-of-order fill cannot keep advertising a full window while the
  hole is dropped. A window-update ACK is sent when the app drains a
  zero window.
- In-order data always wakes `Receive()` (not only PSH / 64KiB).
- `ResendSegment()` does not treat a failed `SendSegment()` as sent.
- DupACKs require an empty segment (`nDataLength == 0`); telnet
  payloads with `ACK == UNA` no longer trigger fast retransmit.
- FIN is subtracted from `nBytesAck` only when the ACK covers `SND.NXT`
  and FIN was actually sent.
- A 1-byte persist probe is sent when the peer advertises a zero window.

## `circle-usb-cdc-rx.patch`

Applied after `circle-tcp-ack.patch` (marker `mmbasic-usb-cdc-rx` in
`lib/usb/usbcdcethernet.cpp`). QEMU `-device usb-net` and USB CDC
Ethernet adapters:

- Two bulk-IN buffers (ping-pong). Completion immediately posts the
  next URB instead of waiting for `ReceiveFrame`, so a synchronous
  `SendFrame` cannot open a receive gap.
- A completed frame is held until `ReceiveFrame` copies it; the other
  slot stays armed.

Do not commit a dirty Circle submodule; the parent tree only vendors the
patch files.
