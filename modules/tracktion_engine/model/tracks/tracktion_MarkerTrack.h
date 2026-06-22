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

/**
    A single, special ClipTrack that holds the Edit's MarkerClips — the named timeline
    locations used for navigation, arrangement sections and loop/jump points.

    There is conventionally one MarkerTrack per Edit and it is not a signal-producing
    track: canContainMarkers() is true, it accepts no audio/MIDI and produces no output.
    It reuses ClipTrack only for the ordered, start-time-sorted clip storage; the clips
    it holds are MarkerClips rather than audio or MIDI.

    @see ClipTrack, MarkerClip, MarkerManager
*/
class MarkerTrack  : public ClipTrack
{
public:
    MarkerTrack (Edit&, const juce::ValueTree&);
    ~MarkerTrack() override;

    bool isMarkerTrack() const override;
    juce::String getSelectableDescription() override;
    juce::String getName() const override;
    bool canContainPlugin (Plugin*) const override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerTrack)
};

}} // namespace tracktion { inline namespace engine
