# Roadmap

Building the **video part first** (per Harrison). MVP = Phases 0–5.

## Phase 0 — Foundation + video spike  ✅ DONE (2026-06-22)
- [x] Toolchain confirmed (clang 17, CMake 4.3.3, git, Homebrew). Full Xcode not installed (CLT is enough to build).
- [x] Project scaffold + git repo + docs.
- [x] Fetch deps: JUCE, foleys_video_engine, VideoExamples, FFmpeg.
- [x] Native JUCE app builds + launches a window on this Mac (CMake + CLT, no full Xcode).
- [x] **Goal met:** a 1080p H.264 video plays + frame-accurate scrubs in a native window via
      AVFoundation/VideoToolbox (`src/VideoView.mm`). Timeline UI itself comes in Phase 1.

## Phase 1 — App shell  ← CURRENT
- Our own app target named **Layback Station**: main window, transport bar, timeline view.
- **Command system** (`ApplicationCommandManager`) + **keymap profiles** (Logic / Pro Tools / Ableton / LS
  default), switchable in Preferences. Every action routes through commands — no hardcoded keys.
- Project open/save using an OTIO-backed model (stub is fine).

## Phase 2 — Multi-video timeline
- Multiple video tracks/clips; proxy media on import; scrub several videos responsively.

## Phase 3 — Candidate-audio stacks
- Per-video collapsible group (nested OTIO stack) holding N candidate songs.
- Import audio, audition, solo, mute non-winners.

## Phase 4 — Music-to-picture editor (the magic)
- Auto scene-cut markers (video) + auto beat/transient markers (audio).
- **Dual snapping:** song beats snap to video cuts. Slide-to-align.
- Trim head/tail + auto fades (intro/outro). Beat-aware split + auto-crossfade (internal cuts).

## Phase 5 — Export
- Per-video render + mux via AVFoundation (ProRes via Apple's encoder; H.264/HEVC via OS encoder).
- Project bundle export (OTIO + copied media) for handing sessions to teammates.

## Phase 6+ — Depth, then product
- Bring in Tracktion Engine for real mixing/mastering and plugin (VST3/AU) hosting.
- Visual skins per DAW + deeper interaction-model emulation (tool modes, modifier conventions). Color grading. Recording.
- Code signing + notarization pipeline → private team distribution → sellable product.

## Explicitly deferred (NOT in MVP)
Color grading, audio recording, mixing/mastering, VST hosting, real-time multiplayer collaboration.
