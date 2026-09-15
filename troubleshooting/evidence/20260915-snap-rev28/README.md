# Connect Agent snap rev 28 (0.5.3) — device-cert POST evidence, 2026-09-15

Captured via a local reverse proxy (snap configured with
gateway-server-address=http://localhost:8080 pointing at the proxy,
which forwarded to https://gw.golioth.io with the gateway's own client
cert). Trigger: button press on the Tikk (pouch v0.2.0 firmware,
device cert verified byte-identical to the console-issued original).

## What the snap POSTs to .g/device-cert

- 004_*.bin — first session after gateway process start: 361 bytes of
  STACK JUNK: 136 bytes of '-' fill (format-string remnant) followed by
  a tail of a base64 conversion buffer ("...sFqiWrgke0eRfglCq9vWHt4Jp9H
  Hl7i\nzp0tXJG9gvXQoyhbkYJpcfYwvSlXWNccgv9it5gu+qOBgDB+MA4GA1UdDwEB/
  wQEAwIFoDAM" — real X.509 tail in base64, i.e. a PEM conversion
  buffer that lived on that stack).
- 006..018_*.bin — every subsequent session: 361 bytes of repeating
  "04 00" dead-stack fill. Identical sha16 across sessions (67434abb...).

The real cert is 361 bytes of DER starting "30 82 01 61" — none of the
POST bodies contain it.

## Controls that rule out every other link

- Device cert on the board: smpmgr-downloaded and byte-compared with the
  console-issued file — IDENTICAL (cert + key).
- The same cert POSTed directly via curl with the gateway's mTLS creds:
  HTTP 200 (idempotent, twice).
- Server GETs (.g/server-cert) through the same proxy: 200.
- Device firmware: pouch v0.2.0; BLE session reaches the gateway every
  press (passkey auto-pairing); the failure is precisely at the cert
  forward step.

## Root cause in upstream pouch

- 87136d7 2026-07-22 16:54 "gateway: offload device cert cloud upload
  to the gateway work queue" (introduces the async upload)
- snap 0.5.3 / rev 28 built 2026-07-22 19:52 (between the two commits)
- c91c2bf 2026-07-23 16:05 "gateway: fix stack use-after-return in
  pouch_gateway_workq_run_sync" (the fix)

The captured bodies are characteristic of a stack-use-after-return: the
async upload reads a stack buffer after the owning frame returned.

## Request to the gateway team

Rebuild the Connect Agent snap from pouch v0.2.0 (or any revision
>= c91c2bf). With that snap, the same device/cert/server chain should
complete the session: peer connects -> device cert POST (real DER) ->
check-in on the console.

proxy.log has the full request/response log including upstream statuses
(400 for every device-cert POST; 200 for .g/server-cert).
