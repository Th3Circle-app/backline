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
}

//==============================================================================
void TimelineComponent::setGroups (const std::vector<std::unique_ptr<VideoGroup>>* g) { groups = g; repaint(); }
void TimelineComponent::setActiveGroup (int g) { activeGroup = g; repaint(); }
void TimelineComponent::setSelection (int group, int track, int clip) { selGroup = group; selTrack = track; selClip = clip; repaint(); }

void TimelineComponent::setPlayhead (double seconds)
{
    if (! juce::approximatelyEqual (seconds, playhead)) { playhead = seconds; repaint(); }
}

void TimelineComponent::setLoop (bool enabled, double start, double end) { loopEnabled = enabled; loopStart = start; loopEnd = end; repaint(); }
void TimelineComponent::setSnapEnabled (bool b) { snapEnabled = b; }

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
    if (clipDragging && dragPps > 0.0) return dragPps;
    const double tl = timelineLength();
    const int laneW = getWidth() - headerW;
    if (tl <= 0.0 || laneW <= 0) return 0.0;
    return (double) laneW / tl;
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

juce::Rectangle<int> TimelineComponent::sBox (int rowYpos) const { return { headerW - 30, rowYpos + rowHeight / 2 - 9, 22, 18 }; }
juce::Rectangle<int> TimelineComponent::mBox (int rowYpos) const { return { headerW - 56, rowYpos + rowHeight / 2 - 9, 22, 18 }; }
juce::Rectangle<int> TimelineComponent::disclosureRectAt (int rowYpos) const { return { 6, rowYpos + rowHeight / 2 - 8, 16, 16 }; }

//==============================================================================
void TimelineComponent::drawMS (juce::Graphics& g, int rowYpos, bool mute, bool solo)
{
    auto m = mBox (rowYpos), s = sBox (rowYpos);
    g.setColour (mute ? juce::Colour (0xffd64545) : juce::Colour (0xff2a2d33));
    g.fillRoundedRectangle (m.toFloat(), 3.0f);
    g.setColour (mute ? juce::Colours::white : juce::Colour (0xff9aa0a6));
    g.setFont (11.0f);
    g.drawText ("M", m, juce::Justification::centred, false);

    g.setColour (solo ? juce::Colour (0xffd9a441) : juce::Colour (0xff2a2d33));
    g.fillRoundedRectangle (s.toFloat(), 3.0f);
    g.setColour (solo ? juce::Colours::black : juce::Colour (0xff9aa0a6));
    g.drawText ("S", s, juce::Justification::centred, false);
}

void TimelineComponent::paint (juce::Graphics& g)
{
    const int w = getWidth();
    const int h = getHeight();

    g.fillAll (juce::Colour (0xff0d0e11));
    g.setColour (juce::Colour (0xff16181d));
    g.fillRect (0, 0, headerW, h);
    g.setColour (juce::Colour (0xff1b1d22));
    g.fillRect (headerW, 0, w - headerW, rulerHeight);

    const double tl = timelineLength();

    // ruler ticks (only meaningful when a group is active)
    if (tl > 0.0)
    {
        const double niceSteps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = niceSteps[(int) (sizeof (niceSteps) / sizeof (double)) - 1];
        for (double s : niceSteps) { if (tl / s <= 10.0) { step = s; break; } }
        g.setFont (11.0f);
        for (double t = 0.0; t <= tl + 1.0e-6; t += step)
        {
            const int x = (int) xForTime (t);
            g.setColour (juce::Colour (0xff26282e));
            g.drawVerticalLine (x, (float) rulerHeight, (float) h);
            g.setColour (juce::Colour (0xff3a3d44));
            g.drawVerticalLine (x, (float) (rulerHeight - 6), (float) rulerHeight);
            g.setColour (juce::Colour (0xff8a9099));
            g.drawText (mmss (t), x + 3, 3, 52, rulerHeight - 5, juce::Justification::centredLeft, false);
        }
    }

    const auto rows = buildRows();
    for (const auto& row : rows)
    {
        if (row.kind == Row::Video)
        {
            const auto* grp = (*groups)[(size_t) row.group].get();
            const bool active = (row.group == activeGroup);

            g.setColour (active ? juce::Colour (0xff1a2230) : juce::Colour (0xff121419));
            g.fillRect (0, row.y, headerW, rowHeight);
            if (active) { g.setColour (juce::Colour (0xff63b3ed)); g.fillRect (0, row.y, 3, rowHeight); }

            auto dr = disclosureRectAt (row.y).toFloat();
            g.setColour (juce::Colour (0xffb8bdc4));
            juce::Path tri;
            if (grp->expanded) tri.addTriangle (dr.getX(), dr.getY(), dr.getRight(), dr.getY(), dr.getCentreX(), dr.getBottom());
            else               tri.addTriangle (dr.getX(), dr.getY(), dr.getX(), dr.getBottom(), dr.getRight(), dr.getCentreY());
            g.fillPath (tri);

            g.setColour (juce::Colours::white);
            g.setFont (12.0f);
            g.drawText ("VIDEO " + juce::String (row.group + 1), 28, row.y + 7, headerW - 90, 16, juce::Justification::centredLeft, true);
            g.setColour (juce::Colour (0xff8a9099));
            g.setFont (10.5f);
            g.drawText (grp->name, 28, row.y + 25, headerW - 36, 16, juce::Justification::centredLeft, true);

            drawMS (g, row.y, grp->videoMute, grp->videoSolo);

            auto clip = videoClipRectAt (row.y, grp->duration);
            g.setColour (active ? juce::Colour (0xff2b6cb0) : juce::Colour (0xff24405e));
            g.fillRoundedRectangle (clip, 4.0f);
            g.setColour (juce::Colour (0xff63b3ed));
            g.drawRoundedRectangle (clip, 4.0f, 1.0f);
        }
        else if (row.kind == Row::Audio)
        {
            const auto* grp = (*groups)[(size_t) row.group].get();
            const auto* t = grp->tracks[(size_t) row.track].get();

            g.setColour ((row.track % 2 == 0) ? juce::Colour (0xff121419) : juce::Colour (0xff14161c));
            g.fillRect (0, row.y, headerW, rowHeight);
            g.setColour (juce::Colours::white);
            g.setFont (11.0f);
            g.drawText (t->name, 12, row.y + 7, headerW - 70, 32, juce::Justification::centredLeft, true);

            drawMS (g, row.y, t->mute, t->solo);

            for (int j = 0; j < (int) t->clips.size(); ++j)
            {
                const auto& c = t->clips[(size_t) j];
                auto r = clipRectAt (row.y, c);
                g.setColour (juce::Colour (0xff1f7a52));
                g.fillRoundedRectangle (r, 4.0f);
                if (t->thumb != nullptr && t->thumb->getTotalLength() > 0.0)
                {
                    g.setColour (juce::Colour (0xffaef0cf));
                    t->thumb->drawChannels (g, r.reduced (4.0f).toNearestInt(), c.sourceIn, c.sourceIn + c.duration, 0.95f);
                }
                const bool sel = (row.group == selGroup && row.track == selTrack && j == selClip);
                g.setColour (sel ? juce::Colour (0xffeafff4) : juce::Colour (0xff48c79a));
                g.drawRoundedRectangle (r, 4.0f, sel ? 2.0f : 1.0f);

                if (row.group == activeGroup)   // beat ticks
                    for (double b : t->beatMarkers)
                        if (b >= c.sourceIn && b <= c.sourceIn + c.duration)
                        {
                            const int bx = (int) xForTime (c.timelineStart + (b - c.sourceIn));
                            g.setColour (juce::Colour (0x66ffffff));
                            g.drawVerticalLine (bx, (float) (r.getY() + 3), (float) (r.getBottom() - 3));
                        }
            }
        }
        else if (row.kind == Row::Import)
        {
            g.setColour (juce::Colour (0xff16181d));
            g.fillRect (0, row.y, headerW, row.h);
            g.setColour (juce::Colour (0xff5fae86));
            g.setFont (12.0f);
            g.drawText ("+  Import Track", 24, row.y, headerW - 28, row.h, juce::Justification::centredLeft, false);
        }
        else // AddVideo
        {
            g.setColour (juce::Colour (0xff1a1d22));
            g.fillRect (0, row.y, w, row.h);
            g.setColour (juce::Colour (0xff7aa7d6));
            g.setFont (12.5f);
            g.drawText ("+  Add Video", 12, row.y, headerW - 16, row.h, juce::Justification::centredLeft, false);
        }
    }

    g.setColour (juce::Colour (0xff05060a));
    g.drawVerticalLine (headerW, 0.0f, (float) h);

    if (const auto* ag = activeG())   // scene-cut markers (active group)
        for (double cm : ag->cutMarkers)
        {
            const int cx = (int) xForTime (cm);
            if (cx > headerW)
            {
                g.setColour (juce::Colour (0x9933e0ff));
                g.drawVerticalLine (cx, (float) rulerHeight, (float) h);
            }
        }

    if (loopEnabled && loopEnd > loopStart && tl > 0.0)
    {
        const int lx0 = (int) xForTime (loopStart);
        const int lx1 = (int) xForTime (loopEnd);
        g.setColour (juce::Colour (0x66ffd166)); g.fillRect (lx0, 0, lx1 - lx0, rulerHeight);
        g.setColour (juce::Colour (0x14ffd166)); g.fillRect (lx0, rulerHeight, lx1 - lx0, h - rulerHeight);
        g.setColour (juce::Colour (0xffffd166));
        g.drawVerticalLine (lx0, 0.0f, (float) h);
        g.drawVerticalLine (lx1, 0.0f, (float) h);
    }

    if (tl > 0.0)
    {
        const int px = (int) xForTime (playhead);
        if (px >= headerW)
        {
            g.setColour (juce::Colour (0xffff5a3c));
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

void TimelineComponent::seekFromMouse (const juce::MouseEvent& e)
{
    const double t = timeForX ((double) e.x);
    playhead = t;
    repaint();
    if (onSeek) onSeek (t);
}

void TimelineComponent::mouseMove (const juce::MouseEvent& e)
{
    if (e.x < headerW) { setMouseCursor (juce::MouseCursor::NormalCursor); return; }

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

void TimelineComponent::mouseDown (const juce::MouseEvent& e)
{
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
        dragMode = Drag::Loop;
        movedDuringLoop = false;
        loopAnchor = timeForX ((double) e.x);
        return;
    }

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

        const auto* t = (*groups)[(size_t) hit->group]->tracks[(size_t) hit->track].get();
        int hitClip = -1;
        for (int j = (int) t->clips.size() - 1; j >= 0; --j)
            if (clipRectAt (hit->y, t->clips[(size_t) j]).contains (e.position)) { hitClip = j; break; }

        if (hitClip >= 0)
        {
            if (onClipSelected) onClipSelected (hit->group, hit->track, hitClip);

            auto r = clipRectAt (hit->y, t->clips[(size_t) hitClip]);
            const bool left  = ((float) e.x <= r.getX() + edgePx);
            const bool right = ((float) e.x >= r.getRight() - edgePx);
            dragMode      = left ? Drag::TrimLeft : right ? Drag::TrimRight : Drag::Move;
            dragGroup     = hit->group;
            dragTrack     = hit->track;
            dragClip      = hitClip;
            dragStartClip = t->clips[(size_t) hitClip];
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
    if (dragMode == Drag::Loop)
    {
        if (e.getDistanceFromDragStart() > 3)
        {
            if (! movedDuringLoop) { movedDuringLoop = true; if (onEditBegin) onEditBegin(); }
            const double cur = timeForX ((double) e.x);
            loopStart   = juce::jmin (loopAnchor, cur);
            loopEnd     = juce::jmax (loopAnchor, cur);
            loopEnabled = true;
            repaint();
            if (onLoopChanged) onLoopChanged (true, loopStart, loopEnd);
        }
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
            if (snapEnabled) ts = applySnap (ts);
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
    if (dragMode == Drag::Loop)
    {
        dragMode = Drag::None;
        if (! movedDuringLoop) { playhead = loopAnchor; repaint(); if (onSeek) onSeek (loopAnchor); }
        return;
    }

    if (dragMode == Drag::Move || dragMode == Drag::TrimLeft || dragMode == Drag::TrimRight)
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
