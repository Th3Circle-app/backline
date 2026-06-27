# Backline — DAW Feature Gap Audit

A checklist of "what a complete DAW has" (compiled from Reaper, Ardour, and the
Nuendo/Pro Tools/Cubase/Studio One/Bitwig common set) mapped against what Backline
has **today**, prioritized for Backline's actual job: **music-to-picture laybacks**
(drop a locked video, fit library cues to it, mix, deliver). Reference only — not a
commitment to build all of it.

Legend: ✓ have · ◐ partial · ✗ missing · **SKIP** = out of scope for laybacks.

---

## What Backline already has (the core is solid)

- **Transport**: play/stop/return-to-zero/cycle/record, loop playback, frame-accurate timecode (HH:MM:SS:FF) + bars|beats, project fps.
- **Editing**: move/trim/split/resize, copy/paste/duplicate/delete, snap + **snap-to-frame**, nudge, reorder tracks/groups, long/extendable workspace, markers, go-to-timecode (Spot).
- **PT edit modes** (slip/grid/spot/shuffle) + **edit tools** (smart/trim/grab/scrub/zoom) with live cursor feedback.
- **Fades**: in/out with 4 curve shapes, crossfade, clip gain, normalize.
- **Time**: time-stretch (SoundTouch, pitch-preserved), speed fades.
- **Mixer**: per-track fader/pan/mute/solo, dB meters, insert FX chain, **Submix buses** (fader/mute/FX), FX send + aux bus, master, link groups.
- **Plugins**: AU + VST3 hosting, scan/browser, insert chains, plugin editors, stock EQ/Comp/Reverb/Delay/Limiter/Gate.
- **Automation**: volume breakpoint envelopes (read + draw).
- **Recording**: input capture to a track.
- **Stem splitter** (Demucs).
- **Video/post**: synced pop-out viewer (rate-chase), film slide + lock, film-audio track, on-clip filmstrip, scene-cut detection, **video export/mux** (mix back to picture).
- **Project**: .lbproj save/load, autosave + crash recovery, missing-media handling, snapshot undo.
- **4 skins** (Logic/Pro Tools/Ableton/Backline), each with its own transport/inspector/feel.

That covers ~13 of Reaper's "15 non-negotiables." The honest gaps below are what a pro would notice.

---

## P1 — Genuinely matters for laybacks, and we're missing it

| Gap | State | Why it matters for laybacks | Effort |
|---|---|---|---|
| **Plugin delay compensation (PDC)** | ✗ | The #1 "pro vs toy" line. Any latency-inducing plugin (a limiter/linear-phase EQ) silently shifts that track off picture/out of phase. Without PDC the mix drifts. | M |
| **Loudness (LUFS) + true-peak metering** | ✗ (peak only) | Deliverables have a spec (e.g. -14 LUFS streaming, -23 EBU R128 broadcast). You can't hit a number you can't see. | M |
| **Loudness-normalized export** | ✗ | The actual deliverable: render the mix normalized to a target LUFS / true-peak ceiling. | S |
| **Stems export** | ✗ | Laybacks often hand back stems (music/dialogue/FX) or per-cue stems. One bounce → multiple files by track/bus. | M |
| **Marker ranges / cue list (exportable)** | ◐ (points only) | Hit points / scene cues as named ranges you can navigate and hand off. | S |
| **Pitch-shift (independent of time)** | ✗ | We can stretch length but not transpose a cue to fit a scene's key/mood. | S |
| **Plugin crash isolation** | ✗ | One bad 3rd-party plugin currently takes the whole app down. Real robustness risk in front of a client. | L |
| **Free-warp / stretch markers** | ◐ (whole-clip only) | Pin a downbeat to a frame and warp *around* it — the precise "hit the cut" move, vs. stretching the entire clip uniformly. | L |

## P1.5 — Matters *if* DB's team records/comps (confirm with them)

| Gap | State | Why | Effort |
|---|---|---|---|
| **Input monitoring + punch in/out** | ◐ (basic capture) | Needed for tracking overdubs to picture. | M |
| **Loop-record into takes + comping (lanes/playlists)** | ✗ | Record multiple passes, assemble the best comp. Table stakes if they perform parts. | L |
| **Metronome/click + count-in + tempo map** | ◐ (fixed tempo) | Required to record in time; tempo map if cues follow tempo changes. | M |

## P2 — Nice, not urgent

| Gap | State | Note |
|---|---|---|
| Automation touch/latch/write modes | ◐ (read+draw) | Live fader-ride writing; we draw envelopes now. |
| Plugin-parameter + pan/send automation | ✗ | Automate more than volume. |
| VCA faders, folder tracks, mix groups beyond link | ◐ | Organizational/scaling. |
| Sidechain routing | ✗ | Ducking music under dialogue (could be P1 for laybacks — see note). |
| Ripple edit / strip-silence / consolidate-glue / tab-to-transient | ✗ | Editing speed-ups. |
| Track freeze / bounce-in-place | ✗ | CPU management on big sessions. |
| More export formats (MP3/AIFF/FLAC), dither | ◐ (WAV) | Delivery flexibility. |
| Version snapshots + undo-history list | ◐ (snapshot undo) | Safety/versioning. |
| Project sample-rate/bit-depth UI | ◐ | Expose settings. |
| Multiple video tracks | ✗ (single) | Reference angles / temp vs final picture. Ardour is also single — not a deal-breaker. |
| Control surfaces / OSC (Mackie/HUI) | ✗ | Hardware fader control. |

## SKIP — out of scope for laybacks (don't build unless the market asks)

MIDI / virtual instruments / piano-roll / step sequencer · ADR system (taker/streamers/script reader) · field-recorder metadata matching · Dolby Atmos / immersive / surround panners · 9-pin/Sony P2 machine control · EUCON/HDX DSP hardware · game-audio (Wwise) · cloud collaboration · AAF/OMF/EDL round-trip *(revisit only if clients want to conform from an NLE editor's cut — then **AAF import** becomes P1)*.

---

## Recommended build order (highest leverage first)

1. **Loudness (LUFS + true-peak) metering → loudness-normalized export** — turns Backline into a tool that hits a real deliverable spec. (P1, biggest credibility jump, mostly S+M.)
2. **Stems export** — the other half of a real handoff. (M)
3. **PDC (plugin delay compensation)** — quietly fixes mixes drifting off picture. (M)
4. **Plugin crash isolation** — the one robustness gap that can embarrass a live demo. (L)
5. **Pitch-shift + exportable cue/marker ranges** — both small, both directly useful for fitting cues. (S each)
6. *If DB's team records:* comping + punch + click/count-in (confirm first). (L)

Sources: reaper.fm/about, ardour.org/features + manual, steinberg/avid/presonus feature docs. Compiled 2026-06-27.
