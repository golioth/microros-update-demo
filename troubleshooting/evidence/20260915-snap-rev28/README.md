# Connect Agent snap rev 28 (0.5.3) — device-cert POST evidence, 2026-09-15

Captured via a local reverse proxy (`../../tools/gw-capture-proxy.py`;
snap configured with `gateway-server-address=http://localhost:8080`
pointing at the proxy, which forwarded to `https://gw.golioth.io` with
the gateway's own client cert).

## What the snap POSTs to .g/device-cert

- 004_*.bin — first session after gateway process start: 361 bytes of
  STACK JUNK: 136 bytes of '-' fill followed by a tail of unrelated
  stack text (a PEM/base64 conversion buffer remnant that lived on that
  stack).
- 006..018_*.bin — every subsequent session: 361 bytes of repeating
  "04 00" dead-stack fill. Identical sha16 across sessions.

The real cert is 361 bytes of DER starting "30 82 01 61" — none of the
POST bodies contain it. Body LENGTH matches the DER length, so the
intended body was the raw cert; content was read from a dead stack
frame — a stack-use-after-return in the snap's async device-cert
upload path.

## Lineage note (corrected 2026-09-15)

The snap is built from the telemetry team's PRIVATE repo, not upstream
pouch — both snap revs on this machine (0.5.2/rev26 built Jul 14,
0.5.3/rev28 built Jul 22) contain `forward device certificate` strings
and both fail identically against a v0.2.0 device. Upstream pouch
commits `87136d7` (Jul 22, async device-cert upload) and `c91c2bf`
(Jul 23, stack-use-after-return fix) are suggestive of the same
bug class but are NOT this binary's provenance — the private repo's
history governs. Fix request: rebuild the snap from a tree with the
equivalent of the c91c2bf fix.

## Controls

- Device cert on the Tikk: smpmgr-downloaded and byte-compared with the
  console-issued file — IDENTICAL (cert + key), verified twice.
- Same cert POSTed via curl with the gateway's mTLS creds to
  gw.golioth.io: HTTP 200 (idempotent). Raw DER is the expected format;
  base64 and PEM bodies return 400.
- Server GETs (.g/server-cert) through the same proxy: 200.
- Ble-sessions reached the cert-forward step every button press
  (passkey auto-pairing OK) — failure isolated to the POST body.

## Broader resolution (same day, later)

The 4.00/400s had a SECOND cause beyond the snap: our Tikk device
firmware (pouch v0.2.0 on NCS v3.0.1 / Zephyr 4.0.99-ncs1) corrupts the
device cert in the BLE SAR/notification leg. Proven by the reference
pair: pouch v0.2.0 `ble_gatt` on nRF52840-DK (Zephyr 4.4.0) → pouch
v0.2.0 gateway on FRDM-RW612 (Zephyr 4.4.0) → production cloud:
SESSION COMPLETED end-to-end. See `../connect-agent.md` section 3.

proxy.log has the full request/response log including upstream statuses
(400 for every device-cert POST; 200 for .g/server-cert).