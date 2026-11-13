# Protocol notes

The camera's Wi-Fi API ("gpControl") is unofficial. The reference is
[goprowifihack](https://github.com/KonradIT/goprowifihack). This file records
only what was confirmed on a real HERO7 Black, firmware HD7.01.01.90.00.

## Confirmed on the camera

- The camera answers on `10.5.5.9` as soon as a computer joins its Wi-Fi.
  No Bluetooth handshake and no pairing call were needed.
- `GET /gp/gpControl` returns a JSON schema (about 200 kB) describing every
  mode, setting, option and status field. The programs are driven by it, so
  nothing about settings is hard-coded. The community docs and the schema
  disagree in places (for example the value of "2.7K 4:3"); the schema wins.
- `GET /gp/gpControl/status` returns `{"status": {...}, "settings": {...}}`.
- Status 54 (`storage.remaining_space`) is in kilobytes.
- Status 70 is the battery percentage; status 10 is "recording"; status 8 is "busy".
- The camera sometimes answers HTTP 500 (control port) or 503 (media port
  8080). Both were seen right after a recording stopped, and 500 was seen for
  a "stop" sent while nothing was recording. The working assumption is that
  these mean "busy"; retrying for a few seconds made the failures go away.
- Media list: `GET :8080/gp/gpMediaList`; files: `GET :8080/videos/DCIM/<dir>/<file>`.

## Live preview

1. `GET /gp/gpControl/execute?p1=gpStream&a1=proto_v2&c1=restart`
2. The camera sends MPEG-TS (H.264 video, AAC audio) over UDP to port 8554 of
   the computer that asked.
3. Send the datagram `_GPHD_:0:0:2:0.000000\n` to `10.5.5.9:8554` every 2.5 s,
   or the stream stops.

A player must read past the first key frame before it knows the picture size.
`ffplay -probesize 8192` (found in older examples) shows no picture; 400000 does.
`gopro-gui` skips probing and feeds packets straight to the H.264 decoder.

## Not confirmed yet

- Wake-on-LAN (`gopro wake`).
- Whether the preview continues while recording, in every resolution.
- Control over USB: no documented way exists for this camera generation;
  over USB the HERO7 is a file-transfer (MTP) device.
