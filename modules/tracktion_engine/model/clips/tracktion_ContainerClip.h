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
/**
    A clip that itself contains an ordered collection of other clips and mixes their
    output down to a single audio stream.

    ContainerClip is the clip-level analogue of a submix FolderTrack: by inheriting both
    AudioClipBase (so it behaves as one audio region on its parent track, with fades,
    gain/pan, plugins and proxy rendering) and ClipOwner (so it nests child clips), it
    lets a number of clips be grouped, moved, looped and processed together as a unit.

    The child clips are summed and rendered through this clip's own processing, meaning
    you can drop effects on the container to apply them to the whole group, or warp/loop
    the group as one. The nested timeline is positioned relative to the container's own
    start/offset.

    @see AudioClipBase, ClipOwner, FolderTrack
*/
class ContainerClip  : public AudioClipBase,
                       public ClipOwner
{
public:
    /** Creates a ContainerClip from a given state. @see ClipOwner::insertWaveClip. */
    ContainerClip (const juce::ValueTree&, EditItemID, ClipOwner&);

    /** Destructor. */
    ~ContainerClip() override;

    using Ptr = juce::ReferenceCountedObjectPtr<ContainerClip>;

    //==============================================================================
    /** @internal */
    juce::ValueTree& getClipOwnerState() override;
    /** @internal */
    EditItemID getClipOwnerID() override;
    /** @internal */
    Selectable* getClipOwnerSelectable() override;
    /** @internal */
    Edit& getClipOwnerEdit() override;

    //==============================================================================
    /** @internal */
    juce::File getOriginalFile() const override                 { return {}; }
    /** @internal */
    bool isUsingFile (const AudioFile&) override;

    //==============================================================================
    /** @internal */
    void initialise() override;
    /** @internal */
    void cloneFrom (Clip*) override;

    //==============================================================================
    /** @internal */
    juce::String getSelectableDescription() override;
    /** @internal */
    bool isMidi() const override;

    /** @internal */
    TimeDuration getSourceLength() const override;
    /** @internal */
    HashCode getHash() const override;

    /** @internal */
    void setLoopDefaults() override;
    /** @internal */
    void setLoopRangeBeats (BeatRange) override;

    //==============================================================================
    /** @internal */
    void flushStateToValueTree() override;
    /** @internal */
    void pitchTempoTrackChanged() override;

private:
    //==============================================================================
    juce::ValueTree clipListState;

    void clipCreated (Clip&) override;
    void clipAddedOrRemoved() override;
    void clipOrderChanged() override;
    void clipPositionChanged() override;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContainerClip)
};

}} // namespace tracktion { inline namespace engine
