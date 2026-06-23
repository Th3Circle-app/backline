# Key Decisions & Constraints

Verified by research on 2026-06-22 (claims fact-checked against primary sources). These shape what we can
build and, especially, what we can legally **sell**. Licensing items are "before we sell," not "before we build."

## Foundation
- **Native C++ / JUCE, Mac-first.** Web runtimes can't host VST/AU or composite multi-stream GPU video well.
- **Audio:** Tracktion Engine (the open-source engine behind Waveform 13, Harrison's cited example).
- **Video:** foleys_video_engine (reference) + AVFoundation/VideoToolbox → Metal (real path).
- **Data model:** OpenTimelineIO — **Apache-2.0, free for closed-source commercial use.**

## Dependency licensing
- **VST3 SDK → MIT** as of VST 3.8 (SDK stamp 2025-10-20, announced Oct 2025). Only attribution required;
  no signed Steinberg agreement. Caveat: "VST" name/logo is still a trademark — don't brand as "VST" without
  following Steinberg's trademark rules. (Source: steinberg.net/press/2025/vst-3-8/)
- **JUCE** — dual AGPLv3 / commercial. AGPLv3 would force open-sourcing a distributed app, so a closed-source
  product needs a JUCE licence: **Starter free; Indie $40/mo (≤ $300K rev); Pro $175/mo or $3,500 perpetual.**
  (Source: juce.com/get-juce)
- **Tracktion Engine** — GPLv3 or commercial (built on JUCE). Selling closed-source needs the commercial license.
- **FFmpeg** — ship the **LGPL** build, dynamically linked. **Never** `--enable-gpl` (pulls x264/x265) or
  `--enable-nonfree` (fdk-aac), or the whole app becomes GPL / non-distributable. (Source: ffmpeg.org/legal.html)

## Codec patents (the real landmine for a sold product)
"The software is free" ≠ "the patents are free."
- **H.264/AVC (Via LA):** per-unit fee — first 100,000 units/yr = $0, then $0.20/unit (100K–5M), $0.10/unit
  (>5M), enterprise cap historically ~$9.75M. Applies if you ship your **own** encoder. (Source: via-la.com)
- **H.265/HEVC (Access Advance, now "VCL Advance" after the Dec 2025 Via merger):** real per-unit royalties;
  a 25% rate increase hits **new licensees signing after June 30, 2026.** (Source: accessadvance.com)
- **Mitigation:** use **OS encoders** (VideoToolbox on Mac, Media Foundation on Windows). Apple/Microsoft
  already licensed those codecs, which **materially reduces — does not provably eliminate** — exposure
  (their EULAs don't clearly indemnify third-party apps). Don't bundle our own x264/x265.
- **ProRes:** no per-unit patent-pool royalty, but Apple gates it via a certification program — use
  **AVFoundation's authorized encoder, not FFmpeg's** unauthorized one.
- **AV1:** royalty-free per AOMedia, but Sisvel runs an AV1 pool and Dolby has litigated — not zero-risk.
- **Action: one IP-lawyer consult on codec exposure before charging money.** Everything else above is settled.

## Distribution & signing
- **macOS:** Developer ID signing + notarization (notarytool + stapler, Hardened Runtime on the executable),
  shipped as a signed `.dmg`. Apple Developer Program $99/yr. Mandatory for clean install since macOS 10.15.
- **Windows (later):** Azure Artifact Signing ~$10/mo, no hardware token (US/Canada individuals OK).
  Note: **EV certs no longer bypass SmartScreen** (removed 2024) — don't pay the EV premium for that.
- **Auto-update:** verify a cryptographic signature on every update artifact (Sparkle / minisign-style).

## A/V sync
- Video is locked, no recording-to-picture → **no hard real-time clock sync needed.** Loose preview (snap
  both to playhead) + exact stored offsets + deterministic export mux. Precision unit = **beat-on-cut**.

## Competitive reality (don't overclaim)
- Logic genuinely caps at **one video per project** — confirmed. That's the real gap.
- Reaper **can** hold multiple videos (just a clumsy NLE) — don't claim it can't.
- Resolve Fairlight **is** a real DAW and **can** host VST instruments; it lacks a MIDI sequencer and any
  candidate-song shootout workflow — that's the defensible gap, not "no DAW."

## "Copy Logic" — the legal line
Emulate Logic's workflow/feel with **our own code and art**. Never lift Apple's code or clone its exact
look/icons/branding in a product we sell. The skin system makes "looks like Logic" just one theme.
