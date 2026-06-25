#include "TimelineComponent.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
    juce::String mmss (double s)
    {
        if (s < 0.0) s = 0.0;
        const int total = (int) s;
        return juce::String::formatted ("%d:%02d", total / 60, total % 60);
    }

    juce::Colour labelOn (juce::Colour fill)   // legible text/tick colour over a clip of this colour
    {
        return fill.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white;
    }

    float fadeCurve (float x, int s)   // 0 linear, 1 exponential, 2 s-curve (bell), 3 logarithmic
    {
        x = juce::jlimit (0.0f, 1.0f, x);
        switch (s) { case 1: return x * x; case 2: return x * x * (3.0f - 2.0f * x); case 3: return std::sqrt (x); default: return x; }
    }

    float gainLineY (juce::Rectangle<float> r, float gainDb)   // y of the clip-gain line (centre = 0 dB, +/-18 dB across height)
    {
        const float half = r.getHeight() * 0.5f - 4.0f;
        return r.getCentreY() - juce::jlimit (-1.0f, 1.0f, gainDb / 18.0f) * half;
    }
}

//==============================================================================
void TimelineComponent::setGroups (const std::vector<std::unique_ptr<VideoGroup>>* g) { groups = g; zoomPps = 0.0; refitZoom(); repaint(); }
void TimelineComponent::setActiveGroup (int g) { activeGroup = g; zoomPps = 0.0; refitZoom(); repaint(); }
void TimelineComponent::setSelection (int group, int track, int clip) { selGroup = group; selTrack = track; selClip = clip; repaint(); }

void TimelineComponent::setPlayhead (double seconds)
{
    if (! juce::approximatelyEqual (seconds, playhead)) { playhead = seconds; repaint(); }
}

void TimelineComponent::setLoop (bool enabled, double start, double end) { loopEnabled = enabled; loopStart = start; loopEnd = end; repaint(); }
void TimelineComponent::setSnapEnabled (bool b) { snapEnabled = b; }
void TimelineComponent::setSkin (const Skin& s) { skin = s; headerW = s.logicHeaders ? 300 : 184; repaint(); }

//==============================================================================
int TimelineComponent::numGroups() const { return groups != nullptr ? (int) groups->size() : 0; }

const VideoGroup* TimelineComponent::activeG() const
{
    if (groups != nullptr && activeGroup >= 0 && activeGroup < (int) groups->size())
        return (*groups)[(size_t) activeGroup].get();
    return nullptr;
}

std::vector<TimelineComponent::Row> TimelineComponent::buildRows() const
{
    std::vector<Row> rows;
    int y = rulerHeight;
    for (int g = 0; g < numGroups(); ++g)
    {
        const auto* grp = (*groups)[(size_t) g].get();
        rows.push_back ({ Row::Video, g, -1, y, rowHeight }); y += rowHeight;
        if (grp->expanded)
        {
            for (int t = 0; t < (int) grp->tracks.size(); ++t) { rows.push_back ({ Row::Audio, g, t, y, rowHeight }); y += rowHeight; }
            rows.push_back ({ Row::Import, g, -1, y, importRowH }); y += importRowH;
        }
    }
    rows.push_back ({ Row::AddVideo, -1, -1, y, importRowH });
    return rows;
}

int TimelineComponent::contentHeight() const
{
    const auto rows = buildRows();
    if (rows.empty()) return rulerHeight + importRowH;
    const auto& last = rows.back();
    return last.y + last.h + 6;
}

double TimelineComponent::rawTimelineLength() const
{
    const auto* g = activeG();
    if (g == nullptr) return 0.0;
    double len = g->duration;
    for (const auto& t : g->tracks)
        for (const auto& c : t->clips)
            len = juce::jmax (len, c.timelineEnd());
    return len;
}

double TimelineComponent::timelineLength() const
{
    const bool clipDragging = (dragMode == Drag::Move || dragMode == Drag::TrimLeft || dragMode == Drag::TrimRight);
    if (clipDragging && frozenLen > 0.0) return frozenLen;
    return rawTimelineLength();
}

double TimelineComponent::pixelsPerSecond() const
{
    const bool clipDragging = (dragMode == Drag::Move || dragMode == Drag::TrimLeft || dragMode == Drag::TrimRight);
    if (clipDragging && dragPps > 0.0) return dragPps;     // stable scale during a clip drag
    if (zoomPps > 0.0) return zoomPps;                      // fixed zoom: editing a clip never rescales the view
    // not yet fit to the window: derive a one-time fit from the visible width
    const double tl = timelineLength();
    const int laneW = (viewportW > 0 ? viewportW : getWidth()) - headerW;
    if (tl <= 0.0 || laneW <= 0) return 100.0;
    return (double) laneW / tl;
}

int TimelineComponent::contentWidth() const
{
    const double len = rawTimelineLength();
    return headerW + (int) std::ceil (len * pixelsPerSecond()) + 80;   // +tail so you can scroll past the end
}

void TimelineComponent::refitZoom()
{
    const double len = rawTimelineLength();
    const int laneW = viewportW - headerW;
    if (len > 0.0 && laneW > 0) zoomPps = (double) laneW / len;
}

void TimelineComponent::setViewportWidth (int w)
{
    if (w <= 0 || w == viewportW) return;
    viewportW = w;
    if (zoomPps <= 0.0) refitZoom();   // first layout / new content: fit once, then stay fixed
}

void TimelineComponent::zoomBy (double factor)
{
    if (zoomPps <= 0.0) refitZoom();
    if (zoomPps <= 0.0) zoomPps = 100.0;
    zoomPps = juce::jlimit (1.0, 6000.0, zoomPps * factor);
    repaint();
    if (onZoomChanged) onZoomChanged();
}

void TimelineComponent::zoomToFit()
{
    zoomPps = 0.0;
    refitZoom();
    repaint();
    if (onZoomChanged) onZoomChanged();
}

double TimelineComponent::xForTime (double t) const { return (double) headerW + t * pixelsPerSecond(); }

double TimelineComponent::timeForX (double x) const
{
    const auto pps = pixelsPerSecond();
    if (pps <= 0.0) return 0.0;
    return juce::jlimit (0.0, timelineLength(), (x - headerW) / pps);
}

juce::Rectangle<float> TimelineComponent::videoClipRectAt (int rowYpos, double durationSeconds) const
{
    const float right = juce::jmin ((float) xForTime (durationSeconds), (float) getWidth());
    return { (float) headerW, (float) (rowYpos + 8), juce::jmax (0.0f, right - (float) headerW), (float) (rowHeight - 16) };
}

juce::Rectangle<float> TimelineComponent::clipRectAt (int rowYpos, const AudioClip& c) const
{
    const float x0 = (float) xForTime (c.timelineStart);
    const float x1 = (float) xForTime (c.timelineEnd());
    return { x0, (float) (rowYpos + 8), x1 - x0, (float) (rowHeight - 16) };
}

juce::Rectangle<int> TimelineComponent::mBox (int rowYpos) const
{
    if (skin.logicHeaders) return { 58, rowYpos + rowHeight - 22, 18, 16 };
    if (skin.ptHeaders)    return { headerW - 22, rowYpos + rowHeight / 2 - 8, 16, 16 };
    return { headerW - 56, rowYpos + rowHeight / 2 - 9, 22, 18 };
}
juce::Rectangle<int> TimelineComponent::sBox (int rowYpos) const
{
    if (skin.logicHeaders) return { 76, rowYpos + rowHeight - 22, 18, 16 };
    if (skin.ptHeaders)    return { headerW - 40, rowYpos + rowHeight / 2 - 8, 16, 16 };
    return { headerW - 30, rowYpos + rowHeight / 2 - 9, 22, 18 };
}
juce::Rectangle<int> TimelineComponent::rBox (int rowYpos) const
{
    if (skin.logicHeaders) return { 94, rowYpos + rowHeight - 22, 18, 16 };    // R = 3rd cell (M S R)
    if (skin.ptHeaders)    return { headerW - 58, rowYpos + rowHeight / 2 - 8, 16, 16 };   // PT: rec at left of cluster
    return {};
}
juce::Rectangle<int> TimelineComponent::vBox (int rowYpos) const
{
    if (! skin.logicHeaders) return {};
    return { 156, rowYpos + rowHeight - 21, 108, 14 };
}
juce::Rectangle<int> TimelineComponent::panBox (int rowYpos) const
{
    if (! skin.logicHeaders) return {};
    return { headerW - 16 - 12, rowYpos + rowHeight - 13 - 12, 24, 24 };   // around the drawn pan knob
}
juce::Rectangle<int> TimelineComponent::disclosureRectAt (int rowYpos) const { return { 6, rowYpos + rowHeight / 2 - 8, 16, 16 }; }

//==============================================================================
void TimelineComponent::drawMS (juce::Graphics& g, int rowYpos, bool mute, bool solo)
{
    auto draw = [&] (juce::Rectangle<int> b, const char* txt, bool on, juce::Colour onCol, juce::Colour onTxt)
    {
        auto rf = b.toFloat();
        g.setColour (on ? onCol : skin.control);
        g.fillRoundedRectangle (rf, 3.5f);
        g.setColour (on ? onCol.brighter (0.35f) : skin.control.brighter (0.2f));
        g.drawRoundedRectangle (rf, 3.5f, 1.0f);
        g.setColour (on ? onTxt : skin.muted);
        g.setFont (11.0f);
        g.drawText (txt, b, juce::Justification::centred, false);
    };
    draw (mBox (rowYpos), "M", mute, juce::Colour (0xffd64545), juce::Colours::white);
    draw (sBox (rowYpos), "S", solo, juce::Colour (0xffd9a441), juce::Colours::black);
}

void TimelineComponent::paint (juce::Graphics& g)
{
    const int w = getWidth();
    const int h = getHeight();

    g.fillAll (skin.windowBg);

    // left header column — subtle vertical gradient
    g.setGradientFill (juce::ColourGradient (skin.headerTop,    0.0f, 0.0f,
                                             skin.headerBottom, 0.0f, (float) h, false));
    g.fillRect (0, 0, headerW, h);

    // ruler strip
    g.setColour (skin.ruler);
    g.fillRect (headerW, 0, w - headerW, rulerHeight);

    const double tl = timelineLength();

    // ruler ticks (only meaningful when a group is active)
    if (tl > 0.0 && skin.barsRuler)
    {
        const double barLen = 2.0;                              // 120 BPM, 4/4 -> 1 bar = 2 s
        const int xEnd = (int) xForTime (tl);
        g.setColour (juce::Colour (0xffb89c43));                // gold "used region" band over the ruler
        g.fillRect (headerW, 0, juce::jmax (0, xEnd - headerW), rulerHeight);
        g.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
        int bar = 0;
        for (double t = 0.0; t <= tl + 1.0e-6; t += barLen, ++bar)
        {
            const int x = (int) xForTime (t);
            g.setColour (skin.windowBg.brighter (0.05f));      // faint bar gridline down the lanes
            g.drawVerticalLine (x, (float) rulerHeight, (float) h);
            g.setColour (juce::Colour (0xff6a5a1e));           // bar tick on the gold band
            g.drawVerticalLine (x, (float) (rulerHeight - 8), (float) rulerHeight);
            for (int bt = 1; bt < 4; ++bt)                     // beat ticks
            {
                const int bx = (int) xForTime (t + barLen * bt / 4.0);
                g.setColour (juce::Colour (0x55000000));
                g.drawVerticalLine (bx, (float) (rulerHeight - 4), (float) rulerHeight);
            }
            if (bar % 2 == 0)                                  // label odd bars (1, 3, 5, ...) near-black on gold
            {
                g.setColour (juce::Colour (0xff1c1c1c));
                g.drawText (juce::String (bar + 1), x + 3, 2, 40, rulerHeight - 4, juce::Justification::centredLeft, false);
            }
        }
    }
    else if (tl > 0.0)
    {
        const double niceSteps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = niceSteps[(int) (sizeof (niceSteps) / sizeof (double)) - 1];
        for (double s : niceSteps) { if (tl / s <= 10.0) { step = s; break; } }
        g.setFont (11.0f);
        for (double t = 0.0; t <= tl + 1.0e-6; t += step)
        {
            const int x = (int) xForTime (t);
            g.setColour (skin.ruler.brighter (0.10f));
            g.drawVerticalLine (x, (float) rulerHeight, (float) h);
            g.setColour (skin.muted.darker (0.2f));
            g.drawVerticalLine (x, (float) (rulerHeight - 6), (float) rulerHeight);
            g.setColour (skin.muted);
            g.drawText (mmss (t), x + 4, 3, 52, rulerHeight - 5, juce::Justification::centredLeft, false);
        }
    }

    g.setColour (skin.windowBg.darker (0.5f));   // ruler bottom border
    g.fillRect (headerW, rulerHeight - 1, w - headerW, 1);

    if (const auto* agm = activeG())             // hit-point markers (flag on the ruler + faint line down the lanes)
    {
        for (const auto& mk : agm->markers)
        {
            const float mx = (float) xForTime (mk.time);
            if (mx < (float) headerW - 2.0f || mx > (float) w) continue;
            g.setColour (juce::Colour (0x44e0a93a)); g.drawLine (mx, (float) rulerHeight, mx, (float) h, 1.0f);
            g.setColour (juce::Colour (0xffe0a93a));
            juce::Path flag; flag.addTriangle (mx, 2.0f, mx + 9.0f, 4.5f, mx, 11.0f); g.fillPath (flag);
            g.drawLine (mx, 2.0f, mx, (float) rulerHeight - 1.0f, 1.0f);
            if (mk.name.isNotEmpty())
            { g.setFont (juce::Font (juce::FontOptions().withHeight (9.0f)));
              g.drawText (mk.name, (int) mx + 11, 1, 96, rulerHeight - 2, juce::Justification::centredLeft, false); }
        }
    }

    const auto rows = buildRows();
    for (const auto& row : rows)
    {
        if (row.kind == Row::Video)
        {
            const auto* grp = (*groups)[(size_t) row.group].get();
            const bool active = (row.group == activeGroup);

            if (skin.logicHeaders)   // flat Logic "Movie" global-track row (no VIDEO banner)
            {
                g.setColour (active ? juce::Colour (0xff828282) : skin.rowEven);
                g.fillRect (0, row.y, headerW, rowHeight);
                g.setColour (skin.windowBg.darker (0.2f));
                g.fillRect (0, row.y + rowHeight - 1, headerW, 1);

                auto dr = disclosureRectAt (row.y).toFloat();
                g.setColour (skin.text.darker (0.1f));
                juce::Path tri;
                if (grp->expanded) tri.addTriangle (dr.getX(), dr.getY(), dr.getRight(), dr.getY(), dr.getCentreX(), dr.getBottom());
                else               tri.addTriangle (dr.getX(), dr.getY(), dr.getX(), dr.getBottom(), dr.getRight(), dr.getCentreY());
                g.fillPath (tri);

                juce::Rectangle<float> ic (26.0f, (float) row.y + 7.0f, 26.0f, 26.0f);   // film icon
                g.setColour (juce::Colour (0xff6a6a6a)); g.fillRoundedRectangle (ic, 5.0f);
                g.setColour (juce::Colour (0xff2a2a2a));
                for (int k = 0; k < 4; ++k)
                { g.fillRect (ic.getX() + 2.5f, ic.getY() + 3.0f + (float) k * 6.0f, 3.0f, 3.0f);
                  g.fillRect (ic.getRight() - 5.5f, ic.getY() + 3.0f + (float) k * 6.0f, 3.0f, 3.0f); }

                g.setColour (skin.text);
                g.setFont (juce::Font (juce::FontOptions().withHeight (12.5f)));
                g.drawText ("Movie", 58, row.y + 7, headerW - 64, 16, juce::Justification::centredLeft, true);
                g.setColour (skin.muted);
                g.setFont (juce::Font (juce::FontOptions().withHeight (10.0f)));
                g.drawText (grp->name, 58, row.y + 24, headerW - 64, 14, juce::Justification::centredLeft, true);

                drawMS (g, row.y, grp->videoMute, grp->videoSolo);
            }
            else
            {
                g.setColour (active ? skin.activeRow : skin.rowEven);
                g.fillRect (0, row.y, headerW, rowHeight);
                g.setColour (active ? skin.activeStrip : skin.videoStrip);
                g.fillRect (0, row.y, 3, rowHeight);

                auto dr = disclosureRectAt (row.y).toFloat();
                g.setColour (skin.text.darker (0.1f));
                juce::Path tri;
                if (grp->expanded) tri.addTriangle (dr.getX(), dr.getY(), dr.getRight(), dr.getY(), dr.getCentreX(), dr.getBottom());
                else               tri.addTriangle (dr.getX(), dr.getY(), dr.getX(), dr.getBottom(), dr.getRight(), dr.getCentreY());
                g.fillPath (tri);

                g.setColour (active ? skin.text : skin.text.darker (0.15f));
                g.setFont (12.0f);
                g.drawText (skin.ptHeaders ? juce::String ("Movie") : ("VIDEO " + juce::String (row.group + 1)), 28, row.y + 7, headerW - 90, 16, juce::Justification::centredLeft, true);
                g.setColour (skin.muted);
                g.setFont (10.5f);
                g.drawText (grp->name, 28, row.y + 25, headerW - 36, 16, juce::Justification::centredLeft, true);

                drawMS (g, row.y, grp->videoMute, grp->videoSolo);
            }

            auto clip = videoClipRectAt (row.y, grp->duration);
            if (clip.getWidth() > 1.0f)
            {
                const auto base = active ? skin.videoClip.brighter (0.12f) : skin.videoClip.darker (0.12f);
                const float rad = skin.flatClips ? 3.0f : 4.0f;
                if (skin.flatClips) { g.setColour (base); g.fillRoundedRectangle (clip, rad); }
                else
                {
                    g.setGradientFill (juce::ColourGradient (base.brighter (0.10f), clip.getX(), clip.getY(),
                                                             base.darker (0.18f),  clip.getX(), clip.getBottom(), false));
                    g.fillRoundedRectangle (clip, rad);
                }
                g.setColour (base.brighter (0.30f));
                g.drawRoundedRectangle (clip, rad, 1.0f);
                if (clip.getWidth() > 64.0f)
                {
                    g.setColour (labelOn (base).withAlpha (0.85f));
                    g.setFont (10.5f);
                    g.drawText (grp->name, clip.toNearestInt().reduced (7, 3), juce::Justification::topLeft, true);
                }

                for (double cm : grp->cutMarkers)   // detected scene cuts, drawn only on this video's clip
                {
                    const int cx = (int) xForTime (cm);
                    if (cx > headerW && (float) cx <= clip.getRight())
                    {
                        g.setColour (juce::Colour (0xcc33e0ff));
                        g.drawVerticalLine (cx, clip.getY() + 1.0f, clip.getBottom() - 1.0f);
                    }
                }
            }
        }
        else if (row.kind == Row::Audio)
        {
            const auto* grp = (*groups)[(size_t) row.group].get();
            const auto* t = grp->tracks[(size_t) row.track].get();

            const auto base     = Skin::clipColour (skin.clipPalette, skin.audioClip,  row.track);
            const auto stripCol = Skin::clipColour (skin.clipPalette, skin.audioStrip, row.track);

            if (skin.tintLanes)                                   // Pro Tools: tint the whole lane its track colour
            { g.setColour (base.withAlpha (0.33f)); g.fillRect (headerW, row.y, w - headerW, rowHeight); }

            const bool selTrk = (row.group == selGroup && row.track == selTrack);
            if (skin.logicHeaders)
            {
                g.setColour (selTrk ? juce::Colour (0xff828282) : ((row.track % 2 == 0) ? skin.rowEven : skin.rowOdd));
                g.fillRect (0, row.y, headerW, rowHeight);
                g.setColour (skin.windowBg.darker (0.2f));
                g.fillRect (0, row.y + rowHeight - 1, headerW, 1);   // row separator

                g.setColour (juce::Colour (0xffc8c8c8));             // track number
                g.setFont (juce::Font (juce::FontOptions().withHeight (12.0f)));
                g.drawText (juce::String (row.track + 1), 0, row.y, 22, rowHeight, juce::Justification::centred, false);

                juce::Rectangle<float> icon (26.0f, (float) row.y + 7.0f, 26.0f, 26.0f);   // flat light-blue colour icon
                g.setColour (juce::Colour (0xff5a9ce0));
                g.fillRoundedRectangle (icon, 5.0f);
                g.setColour (juce::Colours::white.withAlpha (0.92f));
                for (int k = 0; k < 5; ++k)
                {
                    const float bx = icon.getX() + 5.0f + (float) k * 4.0f;
                    const float hh = (k % 2 ? 7.0f : 12.0f);
                    g.fillRect (bx, icon.getCentreY() - hh / 2.0f, 2.0f, hh);
                }

                g.setColour (skin.text);                            // track name (upper)
                g.setFont (juce::Font (juce::FontOptions().withHeight (12.5f)));
                g.drawText (t->name, 58, row.y + 7, headerW - 64, 16, juce::Justification::centredLeft, true);

                {                                                                   // butted M S R segmented group (all wired)
                    juce::Rectangle<int> grp { 58, row.y + rowHeight - 22, 54, 16 };
                    g.setColour (skin.control.darker (0.22f)); g.fillRoundedRectangle (grp.toFloat(), 3.0f);
                    g.setColour (skin.windowBg.darker (0.15f)); g.drawRoundedRectangle (grp.toFloat(), 3.0f, 1.0f);

                    auto cell = [&] (int idx, bool on, juce::Colour col) -> juce::Rectangle<int>
                    {
                        juce::Rectangle<int> b { 58 + idx * 18, row.y + rowHeight - 22, 18, 16 };
                        if (on) { g.setColour (col); g.fillRect (b.reduced (1)); }
                        if (idx > 0) { g.setColour (skin.windowBg.darker (0.1f)); g.fillRect (b.getX(), b.getY() + 2, 1, 12); }
                        return b;
                    };
                    auto lab = [&] (juce::Rectangle<int> b, const char* tx, bool on, juce::Colour col)
                    { g.setColour (on ? juce::Colours::white : col); g.setFont (10.5f); g.drawText (tx, b, juce::Justification::centred, false); };

                    lab (cell (0, t->mute, juce::Colour (0xff4fb0e6)), "M", t->mute, juce::Colour (0xff4fb0e6));            // mute cyan
                    lab (cell (1, t->solo, juce::Colour (0xffe6c84a)), "S", t->solo, juce::Colour (0xffe6c84a));            // solo gold
                    lab (cell (2, t->recordArm, juce::Colour (0xffc8281a)), "R", t->recordArm, juce::Colour (0xffc8281a)); // record red
                }

                auto vb = vBox (row.y);                                              // recessed slider + value fill + cap
                g.setColour (juce::Colour (0xff2e2d2d)); g.fillRoundedRectangle (vb.toFloat(), 3.0f);
                const float vn  = juce::jlimit (0.0f, 1.0f, t->volume / 1.4f);
                const int   thx = vb.getX() + (int) (vn * (float) vb.getWidth());
                g.setColour (juce::Colour (0xff6f7174));
                g.fillRoundedRectangle ((float) vb.getX(), (float) vb.getY(), (float) juce::jmax (0, thx - vb.getX()), (float) vb.getHeight(), 3.0f);
                g.setColour (juce::Colour (0xffc4c6c8));
                g.fillRoundedRectangle ((float) thx - 4.0f, (float) vb.getY() - 2.0f, 8.0f, (float) vb.getHeight() + 4.0f, 2.0f);

                const float pcx = (float) (headerW - 16), pcy = (float) (row.y + rowHeight - 13), pr = 11.0f;   // pan knob
                g.setColour (juce::Colour (0xff3d3d3d)); g.fillEllipse (pcx - pr, pcy - pr, pr * 2.0f, pr * 2.0f);
                g.setColour (juce::Colour (0xff6a6a6a)); g.drawEllipse (pcx - pr, pcy - pr, pr * 2.0f, pr * 2.0f, 1.0f);
                const float pa = juce::jlimit (-1.0f, 1.0f, t->pan) * 2.3f;
                g.setColour (juce::Colour (0xffc8cacb));
                g.drawLine (pcx, pcy, pcx + std::sin (pa) * (pr - 3.0f), pcy - std::cos (pa) * (pr - 3.0f), 1.8f);
            }
            else if (skin.ptHeaders)                               // Pro Tools-style header
            {
                g.setColour (selTrk ? skin.activeRow.brighter (0.08f) : ((row.track % 2 == 0) ? skin.rowEven : skin.rowOdd));
                g.fillRect (0, row.y, headerW, rowHeight);
                g.setColour (skin.windowBg.darker (0.3f));
                g.fillRect (0, row.y + rowHeight - 1, headerW, 1);

                g.setColour (skin.muted);                          // track number
                g.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
                g.drawText (juce::String (row.track + 1), 2, row.y, 18, rowHeight, juce::Justification::centred, false);
                g.setColour (stripCol);                            // wide colour band (matches the region)
                g.fillRect (20, row.y, 6, rowHeight);
                g.setColour (skin.text);                           // name
                g.setFont (juce::Font (juce::FontOptions().withHeight (11.5f)));
                g.drawText (t->name, 32, row.y + 6, headerW - 96, rowHeight - 12, juce::Justification::centredLeft, true);

                auto ptb = [&] (juce::Rectangle<int> b, const char* tx, bool on, juce::Colour col)
                {
                    auto rf = b.toFloat();
                    g.setColour (on ? col : skin.control.darker (0.10f)); g.fillRoundedRectangle (rf, 2.5f);
                    g.setColour (on ? col.brighter (0.30f) : skin.control.brighter (0.12f)); g.drawRoundedRectangle (rf, 2.5f, 1.0f);
                    g.setColour (on ? juce::Colours::white : col); g.setFont (10.0f); g.drawText (tx, b, juce::Justification::centred, false);
                };
                ptb (rBox (row.y), "R", t->recordArm, juce::Colour (0xffc8281a));   // rec = red
                ptb (sBox (row.y), "S", t->solo,      juce::Colour (0xffe6c84a));   // solo = gold
                ptb (mBox (row.y), "M", t->mute,      juce::Colour (0xff4fb0e6));   // mute = blue
            }
            else
            {
                g.setColour ((row.track % 2 == 0) ? skin.rowEven : skin.rowOdd);
                g.fillRect (0, row.y, headerW, rowHeight);
                g.setColour (stripCol);                            // track colour strip
                g.fillRect (0, row.y, 3, rowHeight);
                g.setColour (skin.text);
                g.setFont (11.0f);
                g.drawText (t->name, 12, row.y + 7, headerW - 70, 32, juce::Justification::centredLeft, true);
                drawMS (g, row.y, t->mute, t->solo);
            }

            for (int j = 0; j < (int) t->clips.size(); ++j)
            {
                const auto& c = t->clips[(size_t) j];
                auto r = clipRectAt (row.y, c);
                const bool sel = (row.group == selGroup && row.track == selTrack && j == selClip);
                const float rad = skin.logicHeaders ? 2.0f : (skin.flatClips ? 2.5f : 4.0f);

                // selected regions get the skin's selection fill/outline (Logic: blue body + yellow border);
                // unselected stay the neutral region colour with a contrasting waveform.
                const juce::Colour fillCol    = sel ? (skin.clipSelFill.isTransparent() ? base : skin.clipSelFill) : base;
                const juce::Colour outlineCol = sel ? skin.clipSelOutline : fillCol.brighter (0.32f);
                const juce::Colour waveCol    = sel ? (skin.clipSelFill.isTransparent() ? skin.waveform : juce::Colours::white)
                                                    : (skin.ptHeaders ? skin.waveform
                                                       : (base.getPerceivedBrightness() > 0.52f ? base.darker (0.62f) : skin.waveform));

                if (sel && skin.clipSelFill.isTransparent())   // soft glow for skins without a hard selection colour
                {
                    g.setColour (skin.accent.withAlpha (0.45f));
                    g.drawRoundedRectangle (r.expanded (2.0f), rad + 1.5f, 2.0f);
                }
                if (skin.flatClips || skin.ptHeaders) { g.setColour (fillCol); g.fillRoundedRectangle (r, rad); }
                else
                {
                    g.setGradientFill (juce::ColourGradient (fillCol.brighter (0.10f), r.getX(), r.getY(),
                                                             fillCol.darker (0.20f),  r.getX(), r.getBottom(), false));
                    g.fillRoundedRectangle (r, rad);
                }

                // region title strip with the clip name (DAW-style named regions)
                const bool titled = skin.clipTitleBar && r.getWidth() > 38.0f && r.getHeight() > 24.0f;
                auto waveArea = r.reduced (3.0f);
                if (titled)
                {
                    auto bar = r.withHeight (13.0f);
                    g.setColour (fillCol.darker (sel ? 0.2f : 0.12f).withAlpha (0.95f));
                    g.fillRect (bar.withTrimmedTop (1.0f).reduced (1.0f, 0.0f));
                    g.setColour (sel ? juce::Colours::white : (skin.ptHeaders ? labelOn (fillCol) : juce::Colour (0xff8caae1)));
                    g.setFont (juce::Font (juce::FontOptions().withHeight (10.0f)));
                    g.drawText (t->name, bar.toNearestInt().reduced (5, 0), juce::Justification::centredLeft, true);
                    waveArea = r.withTrimmedTop (13.0f).reduced (3.0f, 2.0f);
                }

                if (t->thumb != nullptr && t->thumb->getTotalLength() > 0.0)
                {
                    g.setColour (waveCol);
                    t->thumb->drawChannels (g, waveArea.toNearestInt(), c.sourceIn, c.sourceIn + c.duration, 0.95f);
                }

                if (r.getWidth() > 24.0f && r.getHeight() > 26.0f)   // clip-gain line (drag to set per-clip gain)
                {
                    const float gy = gainLineY (r, c.gainDb);
                    const bool nonZero = std::abs (c.gainDb) > 0.05f;
                    g.setColour (juce::Colours::white.withAlpha (nonZero ? 0.85f : 0.45f));
                    g.drawLine (r.getX() + 3.0f, gy, r.getRight() - 3.0f, gy, nonZero ? 1.6f : 1.0f);
                    if (nonZero && r.getWidth() > 50.0f)
                    {
                        g.setFont (juce::Font (juce::FontOptions().withHeight (9.0f)));
                        g.drawText ((c.gainDb > 0 ? "+" : "") + juce::String (c.gainDb, 1),
                                    juce::Rectangle<int> ((int) r.getRight() - 40, (int) gy - 11, 37, 10), juce::Justification::centredRight, false);
                    }
                }

                {                                              // fade ramps (curved per shape) + grab handles
                    const float pps = (float) pixelsPerSecond();
                    const float hh  = r.getHeight();
                    if (c.fadeIn > 0.0)
                    {
                        const float fw = juce::jmin ((float) c.fadeIn * pps, r.getWidth());
                        juce::Path fill, line;
                        fill.startNewSubPath (r.getX(), r.getBottom()); line.startNewSubPath (r.getX(), r.getBottom());
                        for (int s = 1; s <= 12; ++s) { const float u = (float) s / 12.0f; const float gx = r.getX() + u * fw; const float gy = r.getBottom() - fadeCurve (u, c.fadeInShape) * hh; fill.lineTo (gx, gy); line.lineTo (gx, gy); }
                        fill.lineTo (r.getX(), r.getY()); fill.closeSubPath();
                        g.setColour (juce::Colours::black.withAlpha (0.32f)); g.fillPath (fill);
                        g.setColour (juce::Colours::white.withAlpha (0.8f));  g.strokePath (line, juce::PathStrokeType (1.3f));
                    }
                    if (c.fadeOut > 0.0)
                    {
                        const float fw = juce::jmin ((float) c.fadeOut * pps, r.getWidth());
                        juce::Path fill, line;
                        fill.startNewSubPath (r.getRight(), r.getBottom()); line.startNewSubPath (r.getRight(), r.getBottom());
                        for (int s = 1; s <= 12; ++s) { const float u = (float) s / 12.0f; const float gx = r.getRight() - u * fw; const float gy = r.getBottom() - fadeCurve (u, c.fadeOutShape) * hh; fill.lineTo (gx, gy); line.lineTo (gx, gy); }
                        fill.lineTo (r.getRight(), r.getY()); fill.closeSubPath();
                        g.setColour (juce::Colours::black.withAlpha (0.32f)); g.fillPath (fill);
                        g.setColour (juce::Colours::white.withAlpha (0.8f));  g.strokePath (line, juce::PathStrokeType (1.3f));
                    }
                    if (r.getWidth() > 30.0f)                   // corner handles (show fades are draggable/resizable)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.55f));
                        g.fillRect (r.getX() + (float) edgePx, r.getY() + 1.0f, 5.0f, 3.0f);
                        g.fillRect (r.getRight() - (float) edgePx - 5.0f, r.getY() + 1.0f, 5.0f, 3.0f);
                    }
                }

                g.setColour (outlineCol);
                g.drawRoundedRectangle (r, rad, sel ? 2.0f : 1.0f);

                if (! titled && r.getWidth() > 52.0f)            // fallback name overlay for narrow clips
                {
                    g.setColour (labelOn (base).withAlpha (0.80f));
                    g.setFont (10.0f);
                    g.drawText (t->name, r.toNearestInt().reduced (6, 2), juce::Justification::topLeft, true);
                }

                if (row.group == activeGroup)   // beat ticks
                    for (double b : t->beatMarkers)
                        if (b >= c.sourceIn && b <= c.sourceIn + c.duration)
                        {
                            const int bx = (int) xForTime (c.timelineStart + (b - c.sourceIn));
                            g.setColour (labelOn (base).withAlpha (0.40f));
                            g.drawVerticalLine (bx, (float) waveArea.getY(), (float) (r.getBottom() - 3));
                        }
            }
        }
        else if (row.kind == Row::Import)
        {
            g.setColour (skin.headerBottom);
            g.fillRect (0, row.y, headerW, row.h);
            g.setColour (juce::Colour (0xff9cc2f2));            // readable, clickable
            g.setFont (12.0f);
            g.drawText ("+  Import Track", 24, row.y, headerW - 28, row.h, juce::Justification::centredLeft, false);
        }
        else // AddVideo
        {
            g.setColour (skin.panel);
            g.fillRect (0, row.y, w, row.h);
            g.setColour (juce::Colour (0xff9cc2f2));
            g.setFont (12.5f);
            g.drawText ("+  Add Video", 12, row.y, headerW - 16, row.h, juce::Justification::centredLeft, false);
        }
    }

    g.setColour (skin.windowBg.darker (0.5f));
    g.drawVerticalLine (headerW, 0.0f, (float) h);

    if (loopEnabled && loopEnd > loopStart && tl > 0.0)
    {
        const int lx0 = (int) xForTime (loopStart);
        const int lx1 = (int) xForTime (loopEnd);
        g.setColour (juce::Colour (0x66ffd166)); g.fillRect (lx0, 0, lx1 - lx0, rulerHeight);
        g.setColour (juce::Colour (0x14ffd166)); g.fillRect (lx0, rulerHeight, lx1 - lx0, h - rulerHeight);
        g.setColour (juce::Colour (0xffffd166));
        g.drawVerticalLine (lx0, 0.0f, (float) h);
        g.drawVerticalLine (lx1, 0.0f, (float) h);
        g.fillRect (lx0 - 2, 0, 4, rulerHeight);   // draggable edge handles
        g.fillRect (lx1 - 2, 0, 4, rulerHeight);
    }

    if (showAutomation)   // volume-automation overlay (amber line + breakpoints) on the active group's audio rows
    {
        const juce::Colour autoCol (0xffffc04d);
        for (const auto& row : rows)
            if (row.kind == Row::Audio && row.group == activeGroup)
            {
                const auto* t = (*groups)[(size_t) row.group]->tracks[(size_t) row.track].get();
                const auto& pts = t->volumeAuto;
                g.setColour (autoCol);
                if (pts.empty())
                {
                    const float yy = autoY (row.y, t->volume);
                    g.drawLine ((float) headerW, yy, (float) w, yy, 1.4f);
                }
                else
                {
                    float px = (float) headerW, py = autoY (row.y, pts.front().value);
                    for (const auto& p : pts) { const float x = (float) xForTime (p.time), y = autoY (row.y, p.value); g.drawLine (px, py, x, y, 1.4f); px = x; py = y; }
                    g.drawLine (px, py, (float) w, py, 1.4f);
                    for (const auto& p : pts) { const float x = (float) xForTime (p.time), y = autoY (row.y, p.value);
                        g.setColour (juce::Colours::white); g.fillRect (x - 3.0f, y - 3.0f, 6.0f, 6.0f); g.setColour (autoCol); }
                }
            }
    }

    if (tl > 0.0)
    {
        const int px = (int) xForTime (playhead);
        if (px >= headerW)
        {
            g.setColour (skin.playhead);
            g.drawVerticalLine (px, 0.0f, (float) h);
            juce::Path tri;
            tri.addTriangle ((float) px - 6.0f, 0.0f, (float) px + 6.0f, 0.0f, (float) px, 9.0f);
            g.fillPath (tri);
        }
    }
}

//==============================================================================
double TimelineComponent::applySnap (double ts) const
{
    if (frameSnap) return juce::jmax (0.0, std::round (ts * 30.0) / 30.0);   // quantize to video frames
    const double pps = (dragPps > 0.0 ? dragPps : pixelsPerSecond());
    if (pps <= 0.0) return ts;
    const double thr = 10.0 / pps;   // 10px snap radius

    const VideoGroup* ag = activeG();
    if (ag == nullptr) return ts;

    double best = thr, bestTs = ts;
    bool found = false;
    auto consider = [&] (double cand) { const double d = std::abs (cand - ts); if (d < best) { best = d; bestTs = cand; found = true; } };

    consider (0.0);                                 // clip start -> timeline 0
    for (double c : ag->cutMarkers) consider (c);    // clip start -> a scene cut

    if (dragTrack >= 0 && dragTrack < (int) ag->tracks.size())
    {
        const auto* t = ag->tracks[(size_t) dragTrack].get();
        for (double b : t->beatMarkers)
        {
            if (b < dragStartClip.sourceIn || b > dragStartClip.sourceIn + dragStartClip.duration) continue;
            const double off = b - dragStartClip.sourceIn;        // beat's offset within the clip
            for (double c : ag->cutMarkers) consider (c - off);   // shift so this beat lands on cut c
        }
    }
    return found ? bestTs : ts;
}

// Shuffle mode: snap the moved clip's start (or end) to a nearby other-clip boundary on the same track.
double TimelineComponent::snapClipToEdges (double ts, double dur) const
{
    if (groups == nullptr || dragGroup < 0 || dragTrack < 0) return ts;
    if (dragGroup >= numGroups() || dragTrack >= (int) (*groups)[(size_t) dragGroup]->tracks.size()) return ts;
    const auto* t = (*groups)[(size_t) dragGroup]->tracks[(size_t) dragTrack].get();
    const double pps = (dragPps > 0.0 ? dragPps : pixelsPerSecond());
    double bestD = (pps > 0.0) ? 14.0 / pps : 0.12, best = ts;
    for (int j = 0; j < (int) t->clips.size(); ++j)
    {
        if (j == dragClip) continue;
        for (double edge : { t->clips[(size_t) j].timelineEnd(), t->clips[(size_t) j].timelineStart })
        {
            if (std::abs (ts - edge) < bestD)         { bestD = std::abs (ts - edge);         best = edge; }        // our start to their edge
            if (std::abs ((ts + dur) - edge) < bestD) { bestD = std::abs ((ts + dur) - edge); best = edge - dur; }  // our end to their edge
        }
    }
    return juce::jmax (0.0, best);
}

void TimelineComponent::seekFromMouse (const juce::MouseEvent& e)
{
    const double t = timeForX ((double) e.x);
    playhead = t;
    repaint();
    if (onSeek) onSeek (t);
}

void TimelineComponent::mouseMove (const juce::MouseEvent& e)
{
    if (groups == nullptr) { setMouseCursor (juce::MouseCursor::NormalCursor); return; }
    if (e.x < headerW) { setMouseCursor (juce::MouseCursor::NormalCursor); return; }

    if (e.y < rulerHeight)   // ruler: show a resize cursor when hovering an existing loop edge
    {
        if (loopEnabled && loopEnd > loopStart)
        {
            const double cur  = timeForX ((double) e.x);
            const double pps  = pixelsPerSecond();
            const double grab = (pps > 0.0) ? 7.0 / pps : 0.0;
            if (std::abs (cur - loopEnd) <= grab || std::abs (cur - loopStart) <= grab)
            { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
        }
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    const auto rows = buildRows();
    for (const auto& row : rows)
    {
        if (row.kind == Row::Audio && e.y >= row.y && e.y < row.y + row.h)
        {
            const auto* t = (*groups)[(size_t) row.group]->tracks[(size_t) row.track].get();
            for (int j = (int) t->clips.size() - 1; j >= 0; --j)
            {
                auto r = clipRectAt (row.y, t->clips[(size_t) j]);
                if (r.contains (e.position))
                {
                    const bool edge = ((float) e.x <= r.getX() + edgePx) || ((float) e.x >= r.getRight() - edgePx);
                    setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);
                    return;
                }
            }
        }
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TimelineComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())          // Cmd/Ctrl + scroll = horizontal zoom
    {
        if (w.deltaY > 0.0f)      zoomBy (1.0f + juce::jmin (0.5f,  w.deltaY));
        else if (w.deltaY < 0.0f) zoomBy (1.0f / (1.0f + juce::jmin (0.5f, -w.deltaY)));
        return;
    }
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())  // otherwise let the viewport scroll
        vp->mouseWheelMove (e.getEventRelativeTo (vp), w);
}

void TimelineComponent::mouseDown (const juce::MouseEvent& e)
{
    if (groups == nullptr) return;
    if (e.mods.isPopupMenu())   // right-click / ctrl-click
    {
        if (e.x >= headerW) { if (onLoopMenu) onLoopMenu (timeForX ((double) e.x)); return; }

        const auto rows = buildRows();   // header column -> track/video context menu
        for (const auto& row : rows)
            if (e.y >= row.y && e.y < row.y + row.h)
            {
                if (row.kind == Row::Audio && onTrackMenu) onTrackMenu (row.group, row.track);
                else if (row.kind == Row::Video && onGroupMenu) onGroupMenu (row.group);
                break;
            }
        return;
    }

    const auto rows = buildRows();

    // locate the row under the cursor
    const Row* hit = nullptr;
    for (const auto& row : rows)
        if (e.y >= row.y && e.y < row.y + row.h) { hit = &row; break; }

    // ---- header column ----
    if (e.x < headerW)
    {
        if (hit == nullptr) return;
        if (hit->kind == Row::Video)
        {
            if (disclosureRectAt (hit->y).contains (e.getPosition())) { if (onToggleExpand) onToggleExpand (hit->group); return; }
            if (mBox (hit->y).contains (e.getPosition())) { if (onVideoMute) onVideoMute (hit->group); return; }
            if (sBox (hit->y).contains (e.getPosition())) { if (onVideoSolo) onVideoSolo (hit->group); return; }
            if (onActivateGroup) onActivateGroup (hit->group);
        }
        else if (hit->kind == Row::Audio)
        {
            if (mBox (hit->y).contains (e.getPosition())) { if (onTrackMute) onTrackMute (hit->group, hit->track); return; }
            if (sBox (hit->y).contains (e.getPosition())) { if (onTrackSolo) onTrackSolo (hit->group, hit->track); return; }
            if (rBox (hit->y).contains (e.getPosition())) { if (onTrackRecord) onTrackRecord (hit->group, hit->track); return; }
            if (vBox (hit->y).contains (e.getPosition()))
            {
                dragMode = Drag::HeaderVol; dragGroup = hit->group; dragTrack = hit->track; dragVBox = vBox (hit->y);
                const float vn = juce::jlimit (0.0f, 1.0f, (float) (e.x - dragVBox.getX()) / (float) juce::jmax (1, dragVBox.getWidth()));
                if (onTrackVolume) onTrackVolume (hit->group, hit->track, vn * 1.4f);
                repaint(); return;
            }
            if (panBox (hit->y).contains (e.getPosition()))
            {
                dragMode = Drag::HeaderPan; dragGroup = hit->group; dragTrack = hit->track; dragStartX = e.x;
                dragStartPan = (groups != nullptr) ? (*groups)[(size_t) hit->group]->tracks[(size_t) hit->track]->pan : 0.0f;
                return;
            }
            if (onActivateGroup) onActivateGroup (hit->group);
            if (onClipSelected)  onClipSelected (hit->group, hit->track, -1);
        }
        else if (hit->kind == Row::Import) { if (onImportTrack) onImportTrack (hit->group); }
        else if (hit->kind == Row::AddVideo) { if (onAddVideo) onAddVideo(); }
        return;
    }

    // ---- ruler = loop strip ----
    if (e.y < rulerHeight)
    {
        if (const auto* agm = activeG())          // click a marker flag -> seek there; double-click -> rename
            for (int i = 0; i < (int) agm->markers.size(); ++i)
            {
                const float mx = (float) xForTime (agm->markers[(size_t) i].time);
                if (std::abs ((float) e.x - mx) <= 6.0f)
                {
                    if (e.getNumberOfClicks() >= 2) { if (onMarkerRename) onMarkerRename (i); }
                    else { playhead = agm->markers[(size_t) i].time; repaint(); if (onSeek) onSeek (playhead); }
                    return;
                }
            }

        dragMode = Drag::Loop;
        movedDuringLoop = false;
        loopResizing = false;
        const double cur = timeForX ((double) e.x);

        loopClickTime = cur;
        if (loopEnabled && loopEnd > loopStart)   // grab the NEARER existing edge -> resize, keeping the far edge fixed
        {
            const double pps    = pixelsPerSecond();
            const double grab   = (pps > 0.0) ? 7.0 / pps : 0.0;   // ~7px grab zone
            const double dEnd   = std::abs (cur - loopEnd);
            const double dStart = std::abs (cur - loopStart);
            if (juce::jmin (dEnd, dStart) <= grab)
            {
                loopAnchor   = (dEnd <= dStart) ? loopStart : loopEnd;   // fix the far edge, drag the near one
                loopResizing = true;
                return;
            }
        }
        loopAnchor = cur;   // otherwise start a fresh region from here
        return;
    }

    // ---- edit-tool overrides (Pro Tools tool palette): Zoom + Scrub act anywhere in the lane ----
    if (editTool == EditTool::Zoom)  { zoomBy (e.mods.isAltDown() ? (1.0 / 1.6) : 1.6); return; }
    if (editTool == EditTool::Scrub) { if (onScrubStart) onScrubStart(); seekFromMouse (e); return; }

    // ---- lane area ----
    if (hit == nullptr) { if (onScrubStart) onScrubStart(); seekFromMouse (e); return; }

    if (hit->kind == Row::Video)
    {
        if (onActivateGroup) onActivateGroup (hit->group);   // clicking a video lane switches to it
        return;
    }

    if (hit->kind == Row::Audio)
    {
        if (hit->group != activeGroup)   // can't edit on the active scale; activate the group first
        {
            if (onActivateGroup) onActivateGroup (hit->group);
            if (onClipSelected)  onClipSelected (hit->group, hit->track, -1);
            return;
        }

        if (showAutomation)   // automation mode: click adds/moves a breakpoint, double-click deletes
        {
            if (onEditBegin) onEditBegin();   // one undo step per automation gesture
            const auto* at = (*groups)[(size_t) hit->group]->tracks[(size_t) hit->track].get();
            dragAuto = at->volumeAuto;
            dragAutoGroup = hit->group; dragAutoTrack = hit->track; dragAutoRowY = hit->y;
            int near = -1;
            for (int i = 0; i < (int) dragAuto.size(); ++i)
            {
                const float x = (float) xForTime (dragAuto[(size_t) i].time), y = autoY (hit->y, dragAuto[(size_t) i].value);
                if (std::abs (x - (float) e.x) <= 6.0f && std::abs (y - (float) e.y) <= 6.0f) { near = i; break; }
            }
            if (near >= 0 && e.getNumberOfClicks() >= 2)   // double-click a point -> delete
            {
                dragAuto.erase (dragAuto.begin() + near);
                if (onAutoEdit) onAutoEdit (dragAutoGroup, dragAutoTrack, dragAuto);
                dragMode = Drag::None; repaint(); return;
            }
            if (near < 0)                                   // empty spot -> add a point and grab it
            {
                AutoPoint np { juce::jmax (0.0, timeForX ((double) e.x)), autoValFromY (hit->y, (float) e.y) };
                dragAuto.push_back (np);
                std::sort (dragAuto.begin(), dragAuto.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.time < b.time; });
                for (int i = 0; i < (int) dragAuto.size(); ++i)
                    if (std::abs (dragAuto[(size_t) i].time - np.time) < 1.0e-9) { near = i; break; }
            }
            dragMode = Drag::AutoPt; dragAutoIdx = near;
            if (onAutoEdit) onAutoEdit (dragAutoGroup, dragAutoTrack, dragAuto);
            repaint(); return;
        }

        const auto* t = (*groups)[(size_t) hit->group]->tracks[(size_t) hit->track].get();
        int hitClip = -1;
        for (int j = (int) t->clips.size() - 1; j >= 0; --j)
            if (clipRectAt (hit->y, t->clips[(size_t) j]).contains (e.position)) { hitClip = j; break; }

        if (hitClip >= 0)
        {
            if (onClipSelected) onClipSelected (hit->group, hit->track, hitClip);

            const auto& cc = t->clips[(size_t) hitClip];
            auto r = clipRectAt (hit->y, cc);
            const float pps0  = (float) pixelsPerSecond();
            const float finW  = (float) cc.fadeIn  * pps0;            // current fade widths -> grab their end to resize
            const float foutW = (float) cc.fadeOut * pps0;
            const bool top   = ((float) e.y <= r.getY() + 12.0f);
            const bool left  = ((float) e.x <= r.getX() + edgePx);
            const bool right = ((float) e.x >= r.getRight() - edgePx);
            const bool fadeInZone  = top && ! left  && (float) e.x <  r.getCentreX() && (float) e.x <= r.getX() + juce::jmax (20.0f, finW)  + 8.0f;
            const bool fadeOutZone = top && ! right && (float) e.x >= r.getCentreX() && (float) e.x >= r.getRight() - juce::jmax (20.0f, foutW) - 8.0f;
            if (editTool == EditTool::Trim)                       // Trimmer: always trim the nearer edge
                dragMode = ((float) e.x < r.getCentreX()) ? Drag::TrimLeft : Drag::TrimRight;
            else if (editTool == EditTool::Grab)                  // Grabber: always move
                dragMode = Drag::Move;
            else                                                 // Smart: zone-based (fade corners / gain line / trim edges / move)
            {
                const float gy = gainLineY (r, cc.gainDb);
                const bool onGain = (! top && ! left && ! right && std::abs ((float) e.y - gy) <= 5.0f);
                dragMode = fadeInZone  ? Drag::FadeIn
                         : fadeOutZone ? Drag::FadeOut
                         : left ? Drag::TrimLeft : right ? Drag::TrimRight
                         : onGain ? Drag::ClipGain : Drag::Move;
            }
            dragGroup     = hit->group;
            dragTrack     = hit->track;
            dragClip      = hitClip;
            dragStartClip = t->clips[(size_t) hitClip];
            dragClipRect  = r;
            dragStartX    = e.x;
            draggedClip   = false;
            frozenLen     = rawTimelineLength();
            const int laneW = getWidth() - headerW;
            dragPps       = (frozenLen > 0.0 && laneW > 0) ? (double) laneW / frozenLen : 0.0;
            repaint();
            return;
        }
    }

    // empty lane -> scrub
    if (onScrubStart) onScrubStart();
    seekFromMouse (e);
}

void TimelineComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (groups == nullptr) return;
    if (dragMode == Drag::HeaderVol)
    {
        const float vn = juce::jlimit (0.0f, 1.0f, (float) (e.x - dragVBox.getX()) / (float) juce::jmax (1, dragVBox.getWidth()));
        if (onTrackVolume) onTrackVolume (dragGroup, dragTrack, vn * 1.4f);
        repaint();
        return;
    }

    if (dragMode == Drag::HeaderPan)
    {
        const float p = juce::jlimit (-1.0f, 1.0f, dragStartPan + (float) (e.x - dragStartX) * 0.012f);   // horizontal drag = pan
        if (onTrackPan) onTrackPan (dragGroup, dragTrack, p);
        repaint();
        return;
    }

    if (dragMode == Drag::AutoPt)
    {
        if (dragAutoIdx >= 0 && dragAutoIdx < (int) dragAuto.size())
        {
            const double lo = (dragAutoIdx > 0) ? dragAuto[(size_t) dragAutoIdx - 1].time + 1.0e-3 : 0.0;
            const double hi = (dragAutoIdx < (int) dragAuto.size() - 1) ? dragAuto[(size_t) dragAutoIdx + 1].time - 1.0e-3 : 1.0e12;
            dragAuto[(size_t) dragAutoIdx].time  = juce::jlimit (lo, hi, timeForX ((double) e.x));
            dragAuto[(size_t) dragAutoIdx].value = autoValFromY (dragAutoRowY, (float) e.y);
            if (onAutoEdit) onAutoEdit (dragAutoGroup, dragAutoTrack, dragAuto);
            repaint();
        }
        return;
    }

    if (dragMode == Drag::ClipGain)
    {
        if (! draggedClip) { draggedClip = true; if (onEditBegin) onEditBegin(); }
        const float half = juce::jmax (1.0f, dragClipRect.getHeight() * 0.5f - 4.0f);
        const float norm = juce::jlimit (-1.0f, 1.0f, (dragClipRect.getCentreY() - (float) e.y) / half);
        AudioClip nc = dragStartClip;
        nc.gainDb = juce::jlimit (-18.0f, 18.0f, norm * 18.0f);
        if (onClipChanged) onClipChanged (dragGroup, dragTrack, dragClip, nc);
        repaint();
        return;
    }

    if (dragMode == Drag::Loop)
    {
        if (e.getDistanceFromDragStart() > 3)
        {
            if (! movedDuringLoop) { movedDuringLoop = true; if (onEditBegin) onEditBegin(); }
            const double cur = timeForX ((double) e.x);
            double s = juce::jmin (loopAnchor, cur), en = juce::jmax (loopAnchor, cur);
            const double minLen = 0.05;               // never collapse the region to zero
            if (en - s < minLen)
            {
                if (cur >= loopAnchor) en = loopAnchor + minLen;
                else                   s  = loopAnchor - minLen;
                if (s < 0.0) { s = 0.0; en = juce::jmax (en, minLen); }
            }
            loopStart = s; loopEnd = en; loopEnabled = true;
            repaint();
            if (onLoopChanged) onLoopChanged (true, loopStart, loopEnd);
        }
        return;
    }

    if (dragMode == Drag::FadeIn || dragMode == Drag::FadeOut)
    {
        if (! draggedClip) { draggedClip = true; if (onEditBegin) onEditBegin(); }
        const double dt = (dragPps > 0.0) ? (double) (e.x - dragStartX) / dragPps : 0.0;
        AudioClip nc = dragStartClip;
        const double maxF = juce::jmax (0.0, dragStartClip.duration * 0.95);
        if (dragMode == Drag::FadeIn) nc.fadeIn  = juce::jlimit (0.0, maxF, dragStartClip.fadeIn  + dt);
        else                          nc.fadeOut = juce::jlimit (0.0, maxF, dragStartClip.fadeOut - dt);
        if (onClipChanged) onClipChanged (dragGroup, dragTrack, dragClip, nc);
        repaint();
        return;
    }

    if (dragMode == Drag::Move || dragMode == Drag::TrimLeft || dragMode == Drag::TrimRight)
    {
        if (! draggedClip) { draggedClip = true; if (onEditBegin) onEditBegin(); }
        const double dt = (dragPps > 0.0) ? (double) (e.x - dragStartX) / dragPps : 0.0;
        AudioClip nc = dragStartClip;

        if (dragMode == Drag::Move)
        {
            double ts = dragStartClip.timelineStart + dt;
            if (shuffle)          ts = snapClipToEdges (ts, dragStartClip.duration);   // Shuffle: butt against neighbours
            else if (snapEnabled) ts = applySnap (ts);
            nc.timelineStart = juce::jmax (0.0, ts);   // clamp AFTER snap (snap candidates can be negative)
        }
        else if (dragMode == Drag::TrimLeft)
        {
            const double lo = juce::jmax (-dragStartClip.sourceIn, -dragStartClip.timelineStart);
            const double hi = juce::jmax (lo, dragStartClip.duration - kMinClipSeconds);
            const double d  = juce::jlimit (lo, hi, dt);
            nc.timelineStart = dragStartClip.timelineStart + d;
            nc.sourceIn      = dragStartClip.sourceIn + d;
            nc.duration      = dragStartClip.duration - d;
        }
        else
        {
            double srcLen = 1.0e9;
            if (groups != nullptr && dragGroup >= 0 && dragGroup < numGroups()
                && dragTrack >= 0 && dragTrack < (int) (*groups)[(size_t) dragGroup]->tracks.size())
                srcLen = (*groups)[(size_t) dragGroup]->tracks[(size_t) dragTrack]->sourceLength;
            const double maxDur = juce::jmax (kMinClipSeconds, srcLen - dragStartClip.sourceIn);
            nc.duration = juce::jlimit (kMinClipSeconds, maxDur, dragStartClip.duration + dt);
        }

        if (onClipChanged) onClipChanged (dragGroup, dragTrack, dragClip, nc);
        repaint();
        return;
    }

    seekFromMouse (e);
}

void TimelineComponent::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == Drag::HeaderVol || dragMode == Drag::HeaderPan) { dragMode = Drag::None; return; }

    if (dragMode == Drag::Loop)
    {
        dragMode = Drag::None;
        if (! movedDuringLoop) { playhead = loopClickTime; repaint(); if (onSeek) onSeek (loopClickTime); }
        loopResizing = false;
        return;
    }

    if (dragMode == Drag::Move || dragMode == Drag::TrimLeft || dragMode == Drag::TrimRight
        || dragMode == Drag::FadeIn || dragMode == Drag::FadeOut)
    {
        const bool wasClick = ! draggedClip;
        const int  cg = dragGroup, ct = dragTrack, cc = dragClip;
        dragMode = Drag::None;
        dragGroup = dragTrack = dragClip = -1;
        if (wasClick && cg >= 0 && ct >= 0 && cc >= 0 && onClipMenu)
            onClipMenu (cg, ct, cc, timeForX ((double) dragStartX));
        repaint();
        return;
    }

    dragMode = Drag::None;
    if (onScrubEnd) onScrubEnd();
}
