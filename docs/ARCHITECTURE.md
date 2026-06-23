# Architecture

Native C++ app built on **JUCE**, macOS-first. One process, one window, like Logic/Resolve.

## Why native (not Electron/Tauri)
- Hosting VST3/AU plugins (needed for the mix/master endgame) is a **C++ ABI** — a web runtime can't do it.
- Multi-stream video decode + GPU compositing + frame-accurate scrubbing is exactly where the browser is
  weakest and native is strongest.
- Compositing native GPU video under a webview is an unsolved, laggy problem (Tauri's own community confirms).

## Layers

### 1. Audio engine
- **Tracktion Engine** (JUCE-based, the engine behind Waveform). Handles transport, multitrack playback,
  recording, plugin hosting. Brought in during the audio phase; not required for the video spike.
- The **audio transport is the timing authority.** Video is displayed against the audio playhead.

### 2. Video engine
- Start from **foleys_video_engine** (JUCE module, FFmpeg-backed, has an NLE example) as a reference.
- Real playback/decode path on macOS: **AVFoundation / VideoToolbox** (hardware decode into GPU memory) →
  **Metal** for display and compositing multiple tracks.
- **Proxy media:** on import, transcode each source to a lightweight proxy and edit against that; only touch
  full-res at export. This is what keeps ~10 videos × ~10 songs responsive.
- Preview compositing (fast, proxy, GPU) and export render (full-res, accurate) are two separate paths.

### 3. Timeline data model — OpenTimelineIO (OTIO)
- `Timeline → Stack → Track → Clip/Gap/Transition`. Nesting = put a `Stack`/`Track` inside a `Track`.
- A clip's in/out is a `source_range` (TimeRange of RationalTime) pointing at a `MediaReference`.
- **Our nested model:** each video = a **nested Stack** holding one video track + N candidate-audio tracks.
  Collapsed = one lane on the main timeline; click in = edit its children. Candidate songs = audio clips
  inside that video's stack.
- **Reference vs. copy:** nested/compound instances that share one definition propagate edits; a duplicate
  is an independent copy. Encode this explicitly so auditioning never mutates another video's edit.
- Serializes to versioned JSON; **OTIOZ/OTIOD file bundles** package timeline + media for portable export.

### 4. A/V sync model (deliberately lightweight)
The video is locked and we never record to picture, so there is **no hard real-time clock-sync problem**:
- **Preview:** snap both video and audio to the playhead on play/seek and let them run. Loose is fine over
  short ads; no continuous drift correction needed.
- **Truth lives in the data + export:** every audio edit is stored as an exact offset. Export re-muxes from
  those offsets, so the result is frame-exact by construction regardless of preview drift.
- Precision unit = **beat-on-cut**, not single-frame lip-sync. Invest precision in edit-time snapping.

### 5. UI, commands & skins — built to feel like the user's DAW
- **Strict engine/UI separation.** The engine exposes commands/state; the UI is a swappable layer.

#### Commands & keymap profiles (DAW muscle memory)
- Every user action is an abstract **command** (Play, Stop, Split-at-playhead, Nudge, Zoom In/Out, …),
  registered with JUCE's `ApplicationCommandManager`. **Nothing is hardcoded to a key.**
- Keys map to commands via a `KeyPressMappingSet`. We ship multiple **keymap profiles**:
  **Layback Station (default), Logic, Pro Tools, Ableton Live.** The user picks one in Preferences and the
  whole keyboard remaps at runtime. Each profile maps each DAW's familiar key to the closest LS command, so a
  Pro Tools / Logic / Ableton user keeps their muscle memory.
- **Discipline: route 100% of actions through the command system from day one.** Retrofitting this is painful.
- **Legal:** key shortcuts are functional and not protected like code or visual trade dress, so shipping
  factory keymap profiles that match each DAW is the *safe* way to feel familiar.
- **Scope:** v1 = key-shortcut profiles. Deeper interaction-model emulation (Pro Tools tool modes / modifier
  conventions, Ableton session-vs-arrangement) is a later layer, not MVP.

#### Visual skins
- A `LookAndFeel` per profile reproduces the *feel* of each DAW over identical functionality. Emulate
  layout/conventions only — never pixel-clone a competitor's exact look/icons/branding.

### 6. Distribution & security
- macOS: Apple **Developer ID signing + notarization** (notarytool + stapler, Hardened Runtime on the exe),
  shipped as a signed `.dmg`. Windows later: **Azure Artifact Signing** (~$10/mo, no token).
- Secure auto-update with signature verification (Sparkle on Mac).
- Project sharing: self-contained bundle (OTIO + copied media), transferred via Drive/Dropbox (files are big).
- Brand footage is often confidential → encrypt project data at rest; design for crypto-erase.

See [DECISIONS.md](DECISIONS.md) for the licensing constraints that shape codec and dependency choices.
