## XRadio v0.6.1 -- Apply no longer crashes X-Plane

One zip for Windows, macOS (Intel + Apple Silicon) and Linux. Unzip into
`X-Plane 12/Resources/plugins/` so you get `plugins/XRadio/`.

The wire protocol is unchanged (v6): a v0.6.1 plugin flies with v0.6.0 pilots and
servers.

### Fixed

- **Apply in the settings crashed X-Plane.** Pressing *Apply* on the Settings tab,
  on Host / Connect, or *Finish* in the first-run wizard ended the sim with
  "This application has crashed because of the plugin: XRadio". The button did
  its work inside the window's own Dear ImGui frame: it saved, restarted the
  built-in server, reopened the audio devices and reconnected right there, and
  while re-applying the theme it switched ImGui contexts and left the overlay's
  one current -- the rest of the main window's frame then ran against a context
  that was not in a frame. A changed UI scale also rebuilt the font atlas while
  the frame still drew with it. Now a button only notes what was asked; the
  flight loop does the work between frames, every context switch is a guard that
  puts the previous context back, the theme is applied by each window in its own
  context at the start of its next frame, and the fonts are rebuilt only when no
  window is mid-frame. The hosted server restarts on a worker thread, so a
  settings change no longer stalls the sim while the old server stops.
- A settings file that could not be written (a sharing violation on Windows,
  over in milliseconds) was saved as nothing, silently. The write is retried,
  and a failure is said in Log.txt and in the window.
- *Reset window position* in the ImGui build brought back the old text window.
- A failed Dear ImGui assert is a line in Log.txt, never the end of the sim.

### New

- **A crash handler on Windows.** If the plugin ever takes X-Plane down again,
  Log.txt says where: `XRadio crash: access violation (0xc0000005) at
  XRadio.xpl+0x1a2b3c`, the address it touched, and a short stack as
  `<module>+<offset>`. X-Plane's own handler runs after it exactly as before.
  The build keeps the symbols beside the plugin (`XRadio.pdb`,
  `XRadio.xpl.debug`, `XRadio.xpl.dSYM`) in the `XRadio-symbols-*` artifacts,
  so an offset becomes a line.

### Tests

- `ui_apply_test`: the whole plugin with its ImGui windows, headless, against a
  fake server -- Apply fifty times (callsign, port, UI scale, theme, language,
  audio devices, hosting) on both tabs and through the wizard, with the cockpit
  overlay and Tower mode's windows open. After every frame the current ImGui
  context is unchanged, no window is left mid-frame, no assert failed, the
  settings reached the file, the fonts were rebuilt between frames and each
  change reconnected exactly once. The v0.6.0 code fails it with a segmentation
  fault; the fix passes.
- `crashlog_test`: the crash handler, including a child process that really
  crashes and leaves the report.

**Full Changelog**: https://github.com/Anviktor2411/xradio/compare/v0.6.0...v0.6.1
