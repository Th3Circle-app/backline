# Vision

## The job it does
A brand sends a finished, locked video. The editor digs through a music library, finds songs
that match the video's vibe and the brief, and syncs music to picture: sliding a song until its
beats land on the scene cuts, then trimming an intro/outro and making a few internal cuts so the
track fits. Today this is done one video at a time in Logic. Layback Station does many videos,
each with many candidate songs, at once.

## Why existing tools don't fit
- **Logic Pro** — great audio, but hard-capped at **one video per project**. Can't audition many ads.
- **Premiere Pro** — great video, weak audio editing.
- **DaVinci Resolve** — great video/color; Fairlight is a real audio page but built for post mixing,
  not for a fast "try 10 songs against this cut" workflow.
- **Reaper** — can technically hold multiple videos, but a clumsy NLE and not built for this.

## The wedge
Layback Station is a **music-supervisor / sync-shootout tool first**, not a "beat Premiere and Logic
at everything" tool. The defensible niche: audition **many candidate songs against many videos fast**,
with beats snapping to scene cuts, then export each. Nobody does that well.

## Core interaction (the magic)
Two marker lanes that snap to each other:
- **Auto scene-cut markers** on each video (frame-difference detection — standard, local, free).
- **Auto beat/transient markers** on each song (onset detection — standard, local, free).
Slide a song and its beats **magnetically snap to the video's cuts**. Internal cuts snap to beats and
auto-crossfade so seams stay musical. This turns "drag by eye until it feels right" into one motion.

## Long-term
Grow into a full creative suite: record / mix / master on the audio side, edit / color / finish on the
video side, in one app — but only after the sync-tool MVP earns it.

## Team
- **Harrison** — video side, product direction.
- **DB**, **Scott** — audio/DAW collaborators. (Note: no in-house DAW exists yet; we build on open source.)
- Skinnable UI so each teammate can work in their preferred DAW's layout (Logic, Pro Tools, …).
