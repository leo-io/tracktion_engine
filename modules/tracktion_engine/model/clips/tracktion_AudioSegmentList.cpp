/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion { inline namespace engine
{

//==============================================================================
// AudioSegmentList turns an AudioClipBase's high-level properties (loop range,
// warp markers, auto-tempo, auto-pitch, speed) into a flat list of Segments.
//
// Each Segment is a contiguous mapping from a region on the Edit timeline
// (start/length, in seconds) to a region of the *source* audio file
// (startSample/lengthSample), plus the stretch ratio and transpose to apply
// while playing it. The playback graph (WaveNode/time-stretch nodes) walks this
// list to know which slice of the file to read for each point in time.
//
// The list is rebuilt whenever the influencing properties change. There are
// three build strategies:
//   - create(... WarpTimeManager ...) : one segment per warp-time region, used
//     when the user has placed warp markers.
//   - buildNormal()    : fixed speed playback; loop iterations become repeated
//     segments, then segments are split at pitch/chord boundaries.
//   - buildAutoTempo() : the clip follows the Edit tempo; segments are laid out
//     in beat-space using the file's loop sync points so the audio stretches to
//     match tempo changes.
//
// Segments may overlap slightly to allow equal-power crossfades at loop joins
// (crossFadeSegments), which masks the click that an abrupt loop wrap produces.
//==============================================================================

/** Debug utility: prints every segment's time range, sample range, and transpose to the JUCE debug output. */
inline void dumpSegments (const juce::Array<AudioSegmentList::Segment>& segments)
{

    DBG ("******************************************");
    for (auto& s : segments)
    {
        juce::String text;

        text += "Start: " + juce::String (s.start.inSeconds()) + "(" + juce::String (s.startSample) + ")\n";
        text += "Length: " + juce::String (s.length.inSeconds()) + "(" + juce::String (s.lengthSample) + ")\n";
        text += "Transpose: " + juce::String (s.transpose) + "\n";
        text += "===============================================";

        DBG(text);
    }
}

//==============================================================================
// The segment's position on the Edit timeline, in seconds.
TimeRange AudioSegmentList::Segment::getRange() const                          { return { start, start + length }; }
// The slice of the source audio file this segment reads, in sample frames.
SampleRange AudioSegmentList::Segment::getSampleRange() const                  { return { startSample, startSample + lengthSample }; }

float AudioSegmentList::Segment::getStretchRatio() const                       { return stretchRatio; }
float AudioSegmentList::Segment::getTranspose() const                          { return transpose; }

bool AudioSegmentList::Segment::hasFadeIn() const                              { return fadeIn; }
bool AudioSegmentList::Segment::hasFadeOut() const                             { return fadeOut; }

// True if nothing plays immediately after this segment (e.g. the last loop
// iteration), so the renderer can fade to silence rather than crossfade.
bool AudioSegmentList::Segment::isFollowedBySilence() const                    { return followedBySilence; }

// Combines all the fields that affect the rendered output so a cached render can
// be invalidated when any of them change. Note start/length (timeline position)
// are deliberately excluded - only the source slice and processing matter.
HashCode AudioSegmentList::Segment::getHashCode() const
{
    return startSample
             ^ (lengthSample * 127)
             ^ (followedBySilence ? 1234 : 5432)
             ^ static_cast<HashCode> (stretchRatio * 1003.0f)
             ^ static_cast<HashCode> (transpose * 117.0f);
}

bool AudioSegmentList::Segment::operator== (const Segment& other) const
{
    return (start           == other.start &&
            length          == other.length &&
            startSample     == other.startSample &&
            lengthSample    == other.lengthSample &&
            stretchRatio    == other.stretchRatio &&
            transpose       == other.transpose &&
            fadeIn          == other.fadeIn &&
            fadeOut         == other.fadeOut);
}

bool AudioSegmentList::Segment::operator!= (const Segment& other) const
{
    return ! operator== (other);
}

//==============================================================================
// Bare constructor: creates an empty list bound to a clip. Callers fill in the
// segments themselves (used by the static warp-time create() overloads).
AudioSegmentList::AudioSegmentList (AudioClipBase& acb) : clip (acb)
{
}

// Full constructor: builds the segment list immediately, optionally applying a
// crossfade between adjacent loop iterations to mask the loop point click.
AudioSegmentList::AudioSegmentList (AudioClipBase& acb, bool relTime, bool shouldCrossfade)
    : clip (acb), relativeTime (relTime)
{
    if (shouldCrossfade)
        crossfadeTime = TimeDuration::fromSeconds (static_cast<double> (clip.edit.engine.getPropertyStorage().getProperty (SettingID::crossfadeBlock, 12.0 / 1000.0)));

    auto& pm = acb.edit.engine.getProjectManager();

    // Only build if at least one valid source exists - the clip's own source
    // file, or any of its takes. Otherwise we'd produce segments pointing at a
    // missing file.
    auto anyTakesValid = [&]
    {
        for (ProjectItemID m : clip.getTakes())
            if (pm.findSourceFile (m).existsAsFile())
                return true;

        return false;
    };

   #if JUCE_DEBUG
    auto f = pm.findSourceFile (clip.getSourceFileReference().getSourceProjectItemID());
    jassert (f == juce::File() || f == clip.getSourceFileReference().getFile());
   #endif

    if (clip.getCurrentSourceFile().existsAsFile() || anyTakesValid())
        build (shouldCrossfade);
}

// Works out the time-stretch factor for a segment: the ratio between how many
// source samples it covers and how many output samples its timeline duration
// occupies. >1 means the source is compressed (plays faster), <1 stretched.
static float calcStretchRatio (const AudioSegmentList::Segment& seg, double sampleRate)
{
    double srcSamples = sampleRate * seg.getRange().getLength().inSeconds();

    if (srcSamples > 0)
        return (float) (seg.getSampleRange().getLength() / srcSamples);

    return 1.0f;
}

// Builds the list from the clip's own loop/tempo/pitch properties (the normal,
// non-warp path). relativeTime offsets the segments so they start at zero rather
// than the clip's edit position.
std::unique_ptr<AudioSegmentList> AudioSegmentList::create (AudioClipBase& acb, bool relativeTime, bool crossFade)
{
    return std::unique_ptr<AudioSegmentList> (new AudioSegmentList (acb, relativeTime, crossFade));
}

// Convenience overload: builds the warp-time based list straight from the clip's
// current WarpTimeManager, wave info and loop info.
std::unique_ptr<AudioSegmentList> AudioSegmentList::create (AudioClipBase& acb)
{
    return create (acb, acb.getWarpTimeManager(), acb.getWaveInfo(), acb.getLoopInfo());
}

std::unique_ptr<AudioSegmentList> AudioSegmentList::create (AudioClipBase& acb, const WarpTimeManager& wtm, const AudioFile& af)
{
    auto wi = af.getInfo();
    return create (acb, wtm, wi, wi.loopInfo);
}

// Warp-time build: produces one segment per warp-time region. The WarpTimeManager
// maps stretched ("warped") timeline positions to positions in the source file,
// so each region becomes a segment whose source range is the un-warped span and
// whose timeline range is the warped span - giving per-region stretch.
std::unique_ptr<AudioSegmentList> AudioSegmentList::create (AudioClipBase& acb, const WarpTimeManager& wtm, const AudioFileInfo& wi, const LoopInfo& li)
{
    std::unique_ptr<AudioSegmentList> asl (new AudioSegmentList (acb));

    CRASH_TRACER
    // The usable span of the source file: the loop in/out markers, defaulting the
    // out marker to the file length when unset (-1).
    auto in  = li.getInMarker();
    auto out = (li.getOutMarker() == -1) ? wi.lengthInSamples : li.getOutMarker();
    jassert (in <= out);

    if (in <= out)
    {
        TimeRange region (std::max (TimePosition(), wtm.getWarpedStart()),
                          wtm.getWarpEndMarkerTime());

        // The warped timeline split into regions between consecutive warp markers.
        // getWarpTimeRegions can touch the file, so it's run via callBlockingCatching.
        juce::Array<TimeRange> warpTimeRegions;
        callBlockingCatching ([&] { warpTimeRegions = wtm.getWarpTimeRegions (region); });
        auto position = warpTimeRegions.size() > 0 ? warpTimeRegions.getUnchecked (0).getStart() : TimePosition();

        for (auto warpRegion : warpTimeRegions)
        {
            // Map this region's warped start/end back to positions in the source file.
            TimeRange sourceRegion (wtm.warpTimeToSourceTime (warpRegion.getStart()),
                                    wtm.warpTimeToSourceTime (warpRegion.getEnd()));

            Segment seg;

            // Source slice (offset by the loop in-marker); timeline slice is the
            // warped length, so stretchRatio falls out of the two lengths.
            seg.startSample    = tracktion::toSamples (sourceRegion.getStart(), wi.sampleRate) + in;
            seg.lengthSample   = tracktion::toSamples (sourceRegion.getEnd(), wi.sampleRate) + in - seg.startSample;
            seg.start          = position;
            seg.length         = warpRegion.getLength();
            seg.stretchRatio   = calcStretchRatio (seg, wi.sampleRate);
            seg.fadeIn         = false;
            seg.fadeOut        = false;
            seg.transpose      = 0.0f;

            // Advance the timeline cursor so the next region butts up against this one.
            position = position + warpRegion.getLength();
            jassert (seg.startSample >= in);
            jassert (seg.startSample + seg.lengthSample <= out);

            asl->segments.add (seg);
        }

        // Apply a short fixed crossfade at the region joins to hide discontinuities.
        asl->crossfadeTime = 0.01s;
        asl->crossFadeSegments();
    }

    return asl;
}

// Two lists are equal if they'd produce the same playback - same crossfade,
// timing mode and segments. Used to skip rebuilds/re-renders when nothing changed.
bool AudioSegmentList::operator== (const AudioSegmentList& other) const noexcept
{
    return crossfadeTime == other.crossfadeTime
            && relativeTime == other.relativeTime
            && segments == other.segments;
}

bool AudioSegmentList::operator!= (const AudioSegmentList& other) const noexcept
{
    return ! operator== (other);
}

// Top-level build dispatcher used by the non-warp constructor. Picks the
// auto-tempo or fixed-speed strategy, then optionally rebases the segments to
// start at zero (relativeTime), which the proxy renderer wants since it works in
// a timeline that starts at the clip, not the Edit.
void AudioSegmentList::build (bool crossfade)
{
    // For chord-track mono auto-pitch, pre-fetch the flattened chord progression
    // so getPitchAt() and the chord-chopping can resolve per-beat transposition.
    if (clip.getAutoPitch() && clip.getAutoPitchMode() == AudioClipBase::chordTrackMono)
        if (auto pg = clip.getPatternGenerator())
            pg->getFlattenedChordProgression (progression, true);

    if (clip.getAutoTempo())
        buildAutoTempo (crossfade);
    else
        buildNormal (crossfade);

    if (relativeTime)
    {
        auto offset = toDuration (getStart());

        for (auto& s : segments)
            s.start = s.start - offset;
    }
}

// Splits an existing segment in two at timeline position 'at', inserting the new
// right-hand half at insertPos. Used when a pitch or chord change falls inside a
// segment, so each side can carry its own transpose value. Source samples are
// divided proportionally to the time split, and a fade is added across the seam.
void AudioSegmentList::chopSegment (Segment& seg, TimePosition at, int insertPos)
{
    Segment newSeg;

    newSeg.start  = at;
    newSeg.length = seg.getRange().getEnd() - newSeg.getRange().getStart();

    newSeg.transpose = getPitchAt (newSeg.start + 0.0001s);
    newSeg.stretchRatio = (float) clip.getSpeedRatio();

    newSeg.fadeIn  = true;
    newSeg.fadeOut = seg.fadeOut;

    newSeg.lengthSample = juce::roundToInt (seg.lengthSample * newSeg.length.inSeconds() / seg.length.inSeconds());
    newSeg.startSample  = seg.getSampleRange().getEnd() - newSeg.lengthSample;

    seg.length = seg.length - newSeg.length;
    seg.lengthSample = newSeg.startSample - seg.startSample;

    seg.fadeOut = true;
    seg.followedBySilence = false;

    jassert (newSeg.length > 0.01s);
    jassert (seg.length > 0.01s);

    segments.insert (insertPos, newSeg);
}

// Fixed-speed build. The source is read at a constant rate (speedRatio); if the
// clip loops, the loop region is repeated as many times as fit within the clip,
// trimming the first/last iterations to the clip bounds.
void AudioSegmentList::buildNormal (bool crossfade)
{
    CRASH_TRACER
    auto wi = clip.getWaveInfo();

    if (wi.sampleRate == 0.0)
        return;

    // Source samples consumed per second of output, taking speed into account.
    auto rate = clip.getSpeedRatio() * wi.sampleRate;
    auto clipPos = clip.getPosition();

    if (clip.isLooping())
    {
        auto clipLoopLen = clip.getLoopLength();

        if (clipLoopLen <= 0s)
            return;

        // The source slice (in samples) that one loop iteration reads.
        auto startSamp  = std::max ((SampleCount) 0, (SampleCount) (rate * clip.getLoopStart().inSeconds()));
        auto lengthSamp = std::max ((SampleCount) 0, (SampleCount) (rate * clipLoopLen.inSeconds()));

        // Emit one segment per loop iteration, walking along the timeline. The
        // clip offset shifts the first iteration's start.
        for (int i = 0; ; ++i)
        {
            auto startTime = clipPos.getStart() + clipLoopLen * i - clipPos.getOffset();

            if (startTime >= clipPos.getEnd())
                break;

            auto end = startTime + clipLoopLen;

            if (end < clipPos.getStart())
                continue;

            Segment seg;

            seg.startSample = startSamp;
            seg.lengthSample = lengthSamp;

            // Trim a partial leading iteration so it begins at the clip start,
            // advancing into the source by the same amount.
            if (startTime < clipPos.getStart())
            {
                auto diff = (SampleCount) ((clipPos.getStart() - startTime).inSeconds() * rate);

                seg.startSample += diff;
                seg.lengthSample -= diff;
                startTime = clipPos.getStart();
            }

            // Trim a partial trailing iteration so it ends at the clip end.
            if (end > clipPos.getEnd())
            {
                auto diff = (SampleCount) ((end - clipPos.getEnd()).inSeconds() * rate);
                seg.lengthSample -= diff;
                end = clipPos.getEnd();
            }

            if (seg.lengthSample <= 0)
                continue;

            seg.start = startTime;
            seg.length = end - startTime;

            seg.transpose = getPitchAt (startTime + 0.0001s);
            seg.stretchRatio = (float) clip.getSpeedRatio();

            seg.fadeIn  = true;
            seg.fadeOut = true;
            seg.followedBySilence = true;

            // If this iteration is contiguous with the previous one, the previous
            // one is no longer "followed by silence" - they join seamlessly.
            if (! segments.isEmpty())
            {
                auto& prev = segments.getReference (segments.size() - 1);

                if (tracktion::abs (prev.getRange().getEnd() - seg.getRange().getStart()) < 0.01s)
                    prev.followedBySilence = false;
            }

            segments.add (seg);
        }

        // Don't fade into the very first or out of the very last iteration.
        if (! segments.isEmpty())
        {
            segments.getReference (0).fadeIn = false;
            segments.getReference (segments.size() - 1).fadeOut = false;
        }
    }
    else
    {
        // Not looped: a single segment reading from the clip offset for the clip length.
        Segment seg;

        seg.start        = clipPos.getStart();
        seg.length       = clipPos.getLength();

        seg.startSample  = juce::jlimit ((SampleCount) 0, wi.lengthInSamples, (SampleCount) (clipPos.getOffset().inSeconds() * rate));
        seg.lengthSample = juce::jlimit ((SampleCount) 0, wi.lengthInSamples, (SampleCount) (clipPos.getLength().inSeconds() * rate));

        seg.transpose    = getPitchAt (clipPos.getStart() + 0.0001s);
        seg.stretchRatio = (float) clip.getSpeedRatio();

        seg.fadeIn       = false;
        seg.fadeOut      = false;

        seg.followedBySilence = true;

        if (seg.length > 0s)
            segments.add (seg);
    }

    // When auto-pitch is on, transposition can change part-way through a segment.
    // Split each segment at every pitch-sequence change that lands inside it (and
    // actually alters the pitch) so each piece carries a single transpose value.
    if (clip.getAutoPitch())
    {
        auto& ps = clip.edit.pitchSequence;

        for (int i = 0; i < ps.getNumPitches(); ++i)
        {
            auto* pitch = ps.getPitch(i);
            jassert (pitch != nullptr);

            auto pitchTm = pitch->getPosition().getStart();

            if (pitchTm > getStart() + 0.01s && pitchTm < getEnd() - 0.01s)
            {
                for (int j = 0; j < segments.size(); ++j)
                {
                    auto& seg = segments.getReference (j);

                    if (seg.getRange().reduced (0.01s).contains (pitchTm)
                         && std::abs (getPitchAt (pitchTm) - getPitchAt (seg.getRange().getStart())) > 0.0001)
                    {
                        chopSegment (seg, pitchTm, j + 1);
                        break;
                    }
                }
            }
        }

        // Likewise split at chord-progression boundaries for chord-track pitching.
        chopSegmentsForChords();
    }

    if (crossfade)
        crossFadeSegments();
}

// Splits segments at each chord change in the flattened progression, so that
// chord-track-driven transposition is constant within each segment.
void AudioSegmentList::chopSegmentsForChords()
{
    if (clip.getAutoPitchMode() == AudioClipBase::chordTrackMono && progression.size() > 0)
    {
        auto& ts = clip.edit.tempoSequence;

        BeatPosition pos;

        for (auto& p : progression)
        {
            auto chordTime = ts.toTime (pos);

            if (chordTime > getStart() + 0.01s && chordTime < getEnd() - 0.01s)
            {
                for (int j = 0; j < segments.size(); ++j)
                {
                    auto& seg = segments.getReference (j);

                    if (seg.getRange().reduced (0.01s).contains (chordTime))
                    {
                        chopSegment (seg, chordTime, j + 1);
                        break;
                    }
                }

            }

            pos = pos + p->lengthInBeats;
        }
    }
}

// Returns the sample positions within 'range' at which the auto-tempo build is
// allowed to start a new segment. These are the file's loop/beat points: either
// the explicit loop points from the LoopInfo, or - if none exist - evenly spaced
// points, one per beat. The range start is always included.
static juce::Array<SampleCount> findSyncSamples (const LoopInfo& loopInfo, SampleRange range)
{
    juce::Array<SampleCount> syncSamples;
    auto numLoopPoints = loopInfo.getNumLoopPoints();

    if (numLoopPoints == 0)
    {
        const auto numBeats = (int) std::ceil (loopInfo.getNumBeats());
        syncSamples.ensureStorageAllocated (numBeats);

        for (int i = 0; i < numBeats; ++i)
            syncSamples.add ((SampleCount) (range.getLength() / (double) numBeats * i + range.getStart() + 0.5));
    }
    else
    {
        for (int i = 0; i < numLoopPoints; ++i)
        {
            auto pos = loopInfo.getLoopPoint (i).pos;

            if (range.contains (pos))
                syncSamples.add (pos);
        }
    }

    if (! syncSamples.contains (range.getStart()))
        syncSamples.add (range.getStart());

    std::sort (syncSamples.begin(), syncSamples.end());
    return syncSamples;
}

// Drops sync points that fall before 'start' (the offset into the loop caused by
// the clip's offset) and makes 'start' the first point, so playback begins part-
// way through the file when the clip is offset.
static juce::Array<SampleCount> trimInitialSyncSamples (const juce::Array<SampleCount>& samples, SampleCount start)
{
    juce::Array<SampleCount> result;
    result.add (start);

    for (auto& s : samples)
        if (s > start)
            result.add (s);

    return result;
}

// Fills in a segment's timeline range from its beat range (via the tempo
// sequence) and derives its stretch ratio and transpose. The sample range must
// already be set by the caller.
void AudioSegmentList::initialiseSegment (Segment& seg, BeatPosition startBeat, BeatPosition endBeat, double sampleRate)
{
    auto& ts = clip.edit.tempoSequence;
    seg.start = ts.toTime (startBeat);
    seg.length = ts.toTime (endBeat) - seg.start;
    seg.stretchRatio = calcStretchRatio (seg, sampleRate);
    seg.fadeIn = false;
    seg.fadeOut = false;
    seg.transpose = getPitchAt (seg.start + 0.0001s);
}

// After the auto-tempo build (which lays out whole loops/beats), trims the result
// to the clip bounds: discards segments fully outside the clip and shortens the
// segments straddling the clip start or end, scaling their source sample range to
// match the new shortened duration.
void AudioSegmentList::removeExtraSegments()
{
    for (int i = segments.size(); --i >= 0;)
    {
        auto& seg = segments.getReference (i);
        auto segTime = seg.getRange();
        auto clipTime = clip.getPosition().time;

        if (! segTime.overlaps (clipTime))
        {
            // Entirely outside the clip - drop it.
            segments.remove(i);
        }
        else if (segTime.getStart() < clipTime.getEnd() && segTime.getEnd() > clipTime.getEnd())
        {
            // Straddles the clip end - shorten the tail.
            auto oldLen       = seg.length;
            seg.length        = getEnd() - seg.start;
            auto ratio        = oldLen / seg.length;
            seg.lengthSample  = static_cast<SampleCount> (seg.lengthSample / ratio + 0.5);
        }
        else if (segTime.getStart() < clipTime.getStart() && segTime.getEnd() > clipTime.getStart())
        {
            // Straddles the clip start - shorten the head and advance into the source.
            auto oldLen       = seg.length;
            auto delta        = getStart() - segTime.getStart();
            seg.start         = seg.start + delta;
            seg.length        = seg.length - delta;
            auto ratio        = oldLen / segTime.getLength();
            auto oldEndSamp   = seg.getSampleRange().getEnd();
            seg.lengthSample  = static_cast<SampleCount> (seg.lengthSample / ratio + 0.5);
            seg.startSample   = oldEndSamp - seg.lengthSample;
        }
    }
}

// Collapses adjacent segments that are really continuous - same stretch and
// transpose, and contiguous in both timeline and source samples - into one. This
// keeps the list compact after the per-beat auto-tempo layout produced many tiny
// segments that don't actually need separate processing.
void AudioSegmentList::mergeSegments (double sampleRate)
{
    for (int i = segments.size() - 1; i >= 1; --i)
    {
        auto& s1 = segments.getReference (i - 1);
        auto& s2 = segments.getReference (i);

        if (std::abs (s1.stretchRatio - s2.stretchRatio) < 0.0001
             && std::abs (s1.transpose - s2.transpose) < 0.0001
             && tracktion::abs (s1.start + s1.length - s2.start) < 0.0001s
             && s1.startSample + s1.lengthSample == s2.startSample)
        {
            s1.length = s1.length + s2.length;
            s1.lengthSample += s2.lengthSample;
            s1.stretchRatio = calcStretchRatio (s1, sampleRate);

            segments.remove (i);
        }
    }
}

// Adds an equal-power crossfade at each join between contiguous segments to mask
// the discontinuity (click) where one loop iteration wraps to the next. A fading-
// out segment is extended by crossfadeTime - reading a little extra source past
// its end - so it overlaps the next segment's fade-in. Segments not followed by a
// contiguous neighbour are marked followedBySilence instead.
void AudioSegmentList::crossFadeSegments()
{
    for (int i = 0; i < segments.size(); ++i)
    {
        auto& s = segments.getReference(i);

        // Fade out into a contiguous next segment by overlapping it.
        if (i < segments.size() - 1
             && (tracktion::abs (s.getRange().getEnd() - segments.getReference (i + 1).start) < 0.0001s))
        {
            auto oldLen = s.length;
            s.fadeOut = true;
            s.length = s.length + crossfadeTime;
            auto ratio = oldLen / s.length;
            s.lengthSample = static_cast<SampleCount> (s.lengthSample / ratio + 0.5);
            s.followedBySilence = false;
        }
        else
        {
            s.followedBySilence = true;
        }

        // Mirror fade-in on a segment whose previous neighbour faded out.
        if (i > 0 && segments.getReference (i - 1).fadeOut)
            s.fadeIn = true;
    }
}

// Auto-tempo build. Here the clip follows the Edit's tempo map, so segments are
// laid out in *beat* space rather than time: each source sync point (beat/loop
// point) is placed at the beat where it should sound, and the tempo sequence
// converts beats to time. The stretch ratio per segment therefore varies with the
// local tempo, time-stretching the audio to keep it in sync.
//
// The looping case has two phases:
//   1. A first pass that honours the clip's beat offset (so playback can start
//      mid-loop), running up to the end of the first loop.
//   2. A repeating pass that tiles whole loops until the clip length is reached.
// Afterwards the segments are chord-chopped, trimmed to the clip and merged.
void AudioSegmentList::buildAutoTempo (bool crossfade)
{
    CRASH_TRACER
    auto wi = clip.getWaveInfo();
    auto& li = clip.getLoopInfo();

    // Usable source span, defaulting the out-marker to the file end.
    SampleRange range (li.getInMarker(),
                       li.getOutMarker() == -1 ? wi.lengthInSamples
                                               : li.getOutMarker());

    if (range.isEmpty())
        return;

    auto& ts = clip.edit.tempoSequence;
    auto syncSamples = findSyncSamples (li, range);
    auto clipStartBeat = clip.getStartBeat();

    if (clip.isLooping())
    {
        auto loopLengthBeats = clip.getLoopLengthBeats();

        if (loopLengthBeats == BeatDuration())
            return;

        // Reduce the clip's beat offset into the range [0, loopLength) so we can
        // start the first loop part-way through.
        auto offsetBeat = clip.getOffsetInBeats();

        while (offsetBeat > loopLengthBeats)
            offsetBeat = offsetBeat - loopLengthBeats;

        if (tracktion::abs (offsetBeat).inBeats() < 0.00001)
            offsetBeat = BeatDuration();

        auto loopStartBeat = clip.getLoopStartBeats() + offsetBeat;

        // Source sample at which the offset loop begins, used to drop earlier sync points.
        auto offsetTime   = TimePosition::fromSeconds (loopStartBeat.inBeats() / li.getBeatsPerSecond (wi));
        auto offsetSample = tracktion::toSamples (offsetTime, wi.sampleRate) + range.getStart();

        auto syncSamplesSubset = trimInitialSyncSamples (syncSamples, offsetSample);

        BeatPosition beatPos;
        BeatPosition loopEndBeat = toPosition (loopLengthBeats) - offsetBeat;

        // Phase 1: emit segments for the (possibly offset) first loop. One segment
        // per sync point; the final partial segment is clipped to loopEndBeat.
        for (int i = 0; i < syncSamplesSubset.size(); ++i)
        {
            Segment seg;

            seg.startSample  = syncSamplesSubset[i];
            seg.lengthSample = ((i == syncSamplesSubset.size() - 1) ? (range.getEnd() - seg.startSample)
                                                                    : (syncSamplesSubset[i + 1]) - seg.startSample);

            auto startBeat = beatPos;
            beatPos = beatPos + BeatDuration::fromBeats (TimeDuration::fromSamples (seg.lengthSample, wi.sampleRate).inSeconds() * li.getBeatsPerSecond (wi));
            auto endBeat = beatPos;

            initialiseSegment (seg, clipStartBeat + toDuration (startBeat), clipStartBeat + toDuration (endBeat), wi.sampleRate);

            if (startBeat >= loopEndBeat)
                break;

            if (endBeat > loopEndBeat)
            {
                auto oldLength = endBeat     - startBeat;
                auto newLength = loopEndBeat - startBeat;

                seg.length = ts.toTime (clipStartBeat + toDuration (loopEndBeat)) - seg.start;
                seg.lengthSample = static_cast<SampleCount> (seg.lengthSample * (newLength / oldLength) + 0.5);

                jassert (seg.startSample >= range.getStart());
                jassert (seg.startSample + seg.lengthSample <= range.getEnd());
                segments.add (seg);
                break;
            }

            jassert (seg.startSample >= range.getStart());
            jassert (seg.startSample + seg.lengthSample <= range.getEnd());
            segments.add (seg);
        }

        // Phase 2: now repeat full loops (no offset) until the clip length is
        // covered. Recompute the sync subset from the true loop start.
        loopStartBeat = clip.getLoopStartBeats();

        offsetTime   = TimePosition::fromSeconds (loopStartBeat.inBeats() / li.getBeatsPerSecond (wi));
        offsetSample = tracktion::toSamples (offsetTime, wi.sampleRate);

        syncSamplesSubset = trimInitialSyncSamples (syncSamples, offsetSample);

        beatPos = loopEndBeat;
        loopEndBeat = beatPos + loopLengthBeats;

        while (beatPos < toPosition (clip.getLengthInBeats()))
        {
            for (int i = 0; i < syncSamplesSubset.size(); ++i)
            {
                Segment seg;

                seg.startSample  = syncSamplesSubset[i];
                seg.lengthSample = ((i == syncSamplesSubset.size() - 1) ? (range.getEnd() - seg.startSample)
                                                                        : (syncSamplesSubset[i + 1]) - seg.startSample);

                auto startBeat = beatPos;
                beatPos = beatPos + BeatDuration::fromBeats ((seg.lengthSample / wi.sampleRate) * li.getBeatsPerSecond (wi));
                auto endBeat = beatPos;

                initialiseSegment (seg, clipStartBeat + toDuration (startBeat), clipStartBeat + toDuration (endBeat), wi.sampleRate);

                if (startBeat >= loopEndBeat)
                    break;

                if (endBeat > loopEndBeat)
                {
                    auto oldLength = endBeat     - startBeat;
                    auto newLength = loopEndBeat - startBeat;

                    seg.length = ts.toTime (clipStartBeat + toDuration (loopEndBeat)) - seg.start;
                    seg.lengthSample = static_cast<SampleCount> (seg.lengthSample * (newLength / oldLength) + 0.5);

                    jassert (seg.startSample >= range.getStart());
                    jassert (seg.startSample + seg.lengthSample <= range.getEnd());
                    segments.add (seg);
                    break;
                }

                jassert (seg.startSample >= range.getStart());
                jassert (seg.startSample + seg.lengthSample <= range.getEnd());
                segments.add (seg);
            }

            beatPos = loopEndBeat;
            loopEndBeat = beatPos + loopLengthBeats;
        }
    }
    else
    {
        // Non-looping auto-tempo: a single pass over the file's sync points, each
        // placed at its corresponding beat, honouring the clip offset.
        auto offsetTime = TimeDuration::fromSeconds (clip.getOffsetInBeats().inBeats() / li.getBeatsPerSecond (wi));
        auto offsetSample = tracktion::toSamples (offsetTime, wi.sampleRate) + range.getStart();
        BeatPosition beatPos;

        syncSamples = trimInitialSyncSamples (syncSamples, offsetSample);

        for (int i = 0; i < syncSamples.size(); ++i)
        {
            Segment seg;

            seg.startSample  = syncSamples[i];
            seg.lengthSample = ((i == syncSamples.size() - 1) ? (range.getEnd() - seg.startSample)
                                                              : (syncSamples[i + 1]) - seg.startSample);

            auto startBeat = beatPos;
            beatPos = beatPos + BeatDuration::fromBeats ((seg.lengthSample / wi.sampleRate) * li.getBeatsPerSecond (wi));
            auto endBeat = beatPos;

            initialiseSegment (seg, clipStartBeat + toDuration (startBeat), clipStartBeat + toDuration (endBeat), wi.sampleRate);

            jassert (seg.startSample >= range.getStart());
            jassert (seg.startSample + seg.lengthSample <= range.getEnd());
            segments.add (seg);
        }
    }

    // Post-process: split for chords, clamp to the clip, then merge redundant splits.
    chopSegmentsForChords();
    removeExtraSegments();
    mergeSegments (wi.sampleRate);

    if (crossfade)
        crossFadeSegments();
}

// The timeline start of the first segment (or zero if empty).
TimePosition AudioSegmentList::getStart() const
{
    if (! segments.isEmpty())
        return segments.getReference (0).getRange().getStart();

    return 0.0s;
}

// The timeline end of the last segment (or zero if empty).
TimePosition AudioSegmentList::getEnd() const
{
    if (! segments.isEmpty())
        return segments.getReference (segments.size() - 1).getRange().getEnd();

    return 0.0s;
}

// Returns the transposition (in semitones) to apply at timeline position t.
// In chord-track mode it resolves the chord active at t against the pitch
// sequence's key/scale to get the root-note offset; in plain auto-pitch mode it
// transposes the file's root note to the current pitch setting. With auto-pitch
// off it just returns the clip's manual pitch-change amount.
float AudioSegmentList::getPitchAt (TimePosition t)
{
    if (clip.getAutoPitch() && clip.getAutoPitchMode() == AudioClipBase::chordTrackMono && progression.size() > 0)
    {
        auto& ts = clip.edit.tempoSequence;

        auto& ps = clip.edit.pitchSequence;
        auto& pitchSetting = ps.getPitchAt (t);

        auto beat = ts.toBeats (t);
        BeatPosition pos;

        for (auto& p : progression)
        {
            if (beat >= pos && beat < pos + p->lengthInBeats)
            {
                int key = pitchSetting.getPitch() % 12;

                auto scale = pitchSetting.getScale();

                if (p->chordName.get().isNotEmpty())
                {
                    int scaleNote = key;
                    int chordNote = p->getRootNote (key, scale);

                    int delta = chordNote - scaleNote;

                    int transposeBase = scaleNote - (clip.getLoopInfo().getRootNote() % 12);

                    while (transposeBase > 6)  transposeBase -= 12;
                    while (transposeBase < -6) transposeBase += 12;

                    transposeBase += p->octave * 12;

                    return (float) (transposeBase + delta + clip.getTransposeSemiTones (false));
                }
            }

            pos = pos + p->lengthInBeats.get();
        }
    }

    if (clip.getAutoPitch())
    {
        auto& ps = clip.edit.pitchSequence;
        auto& pitchSetting = ps.getPitchAt (t);

        int pitch = pitchSetting.getPitch();
        int transposeBase = pitch - clip.getLoopInfo().getRootNote();

        while (transposeBase > 6)  transposeBase -= 12;
        while (transposeBase < -6) transposeBase += 12;

        return (float) (transposeBase + clip.getTransposeSemiTones (false));
    }

    return clip.getPitchChange();
}

}} // namespace tracktion { inline namespace engine
