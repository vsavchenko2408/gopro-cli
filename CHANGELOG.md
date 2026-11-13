# Changelog

## 0.6.1
- Fixed: on macOS no setting could be changed in `gopro-gui`; the dropdowns
  did not open. Each dropdown's ImGui id came from the memory address of a
  per-frame copy of the camera state. glibc happened to reuse the same address
  every frame, so it worked on Linux; on macOS the address changed between
  mouse press and release, and ImGui ignored the click. Ids now come from the
  setting itself.

## 0.6.0
- Builds on Linux, macOS and Windows: system-specific code moved to `src/platform.*`.
- `gopro preview` no longer uses `fork`; the player runs through the system shell.
- File sizes are parsed as 64-bit (videos over 2 GB on Windows).
- Smoke test against the camera emulator (`ctest`), CI for all three systems.
- Project files: licence, English and Ukrainian README, protocol notes.

## 0.5.0
- `gopro-gui`: live preview decoded in-process, shooting controls, all settings, media.

## 0.4.x
- `gopro preview` through ffplay; fixed the probe size so the picture appears.

## 0.3.0
- `record` and `photo` confirm the result from the camera's state; `--verbose`.

## 0.2.0
- Requests are retried while the camera answers "busy" (HTTP 500/503).

## 0.1.0
- First CLI: status, modes, settings, shutter, media.
