# Layback Station (LS)

A hybrid **video editor + DAW** for music-to-video sync. Drop in finished brand videos,
audition and cut candidate songs against each one, lock the sync, and export — multiple
videos and many candidate tracks at once. (Logic caps at one video per project; this does not.)

> "Layback" is the post-production term for laying final mixed audio back onto the master video.

**Status:** Phase 0 — foundation + video spike. Build started 2026-06-22. Not yet buildable end-to-end.

## Stack
- **Native C++ / JUCE**, macOS-first.
- **Audio engine:** [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) (the open-source engine behind Tracktion Waveform).
- **Video:** [foleys_video_engine](https://github.com/ffAudio/foleys_video_engine) (FFmpeg-backed) as a starting reference; AVFoundation / VideoToolbox → Metal for native decode and playback.
- **Project format:** [OpenTimelineIO](https://opentimelineio.readthedocs.io/) (Apache-2.0).

## Docs
- [docs/VISION.md](docs/VISION.md) — what it is, who it's for, the wedge
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how it's built
- [docs/ROADMAP.md](docs/ROADMAP.md) — phases + MVP scope
- [docs/DECISIONS.md](docs/DECISIONS.md) — key technical/legal calls, with sources

## Build (Phase 0, in progress)
Requires: macOS, Xcode Command Line Tools, CMake, FFmpeg (`brew install ffmpeg`), JUCE.
Dependencies live in `external/` (not committed). See ROADMAP for current step.

## Team
- **Harrison** — video side, product
- **DB**, **Scott** — audio/DAW collaborators
