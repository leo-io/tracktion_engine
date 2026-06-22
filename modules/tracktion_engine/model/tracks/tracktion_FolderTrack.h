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
    A track that groups other tracks beneath it, optionally summing their output as a
    submix bus.

    A FolderTrack has no clips of its own; its children are nested Tracks (held via the
    Track's sub-track list). It serves two roles:

    - Organisational folder: collapse/expand a group of tracks in the GUI with no effect
      on the signal flow.
    - Submix bus: when it owns a TrackOutput (isSubmixFolder() / getOutput()) the audio
      of all descendant tracks is routed into the folder, processed by the folder's
      PluginList, and emitted as one signal. This allows shared effects, a single
      VCA/volume control (getVCAPlugin / getVolumePlugin / getVcaDb) and group muting.

    Mute/solo are aggregated across children (isMuted/isSolo honour the children's and
    destinations' states), and a folder can be group-frozen. For display it can build
    CollectionClips spanning the clips of its child tracks (generateCollectionClips).

    @see Track, MasterTrack, TrackOutput, VCAPlugin
*/
class FolderTrack  : public Track
{
public:
    FolderTrack (Edit&, const juce::ValueTree&);
    ~FolderTrack() override;

    using Ptr = juce::ReferenceCountedObjectPtr<FolderTrack>;

    //==============================================================================
    juce::String getSelectableDescription() override;
    bool isFolderTrack() const override;

    void initialise() override;

    void sanityCheckName() override;
    juce::String getName() const override;

    // not an index - values start from 1
    int getFolderTrackNumber() const noexcept;

    //==============================================================================
    bool isSubmixFolder() const;

    /** Returns the output if this track is a submix folder. */
    TrackOutput* getOutput() const noexcept;

    juce::Array<Track*> getInputTracks() const override;

    //==============================================================================
    bool isFrozen (FreezeType) const override;

    //==============================================================================
    float getVcaDb (TimePosition);
    VCAPlugin* getVCAPlugin();
    VolumeAndPanPlugin* getVolumePlugin();

    //==============================================================================
    void generateCollectionClips (SelectionManager&);
    CollectionClip* getCollectionClip (int index)  const noexcept;
    int getNumCollectionClips() const noexcept;
    int indexOfCollectionClip (CollectionClip*) const;
    int getIndexOfNextCollectionClipAt (TimePosition);
    CollectionClip* getNextCollectionClipAt (TimePosition);
    bool contains (CollectionClip*) const;

    //==============================================================================
    int getNumTrackItems() const override;
    TrackItem* getTrackItem (int idx) const override;
    int indexOfTrackItem (TrackItem*) const override;
    int getIndexOfNextTrackItemAt (TimePosition) override;
    TrackItem* getNextTrackItemAt (TimePosition) override;

    //==============================================================================
    bool isMuted (bool includeMutingByDestination) const override;
    bool isSolo (bool includeIndirectSolo) const override;
    bool isSoloIsolate (bool includeIndirectSolo) const override;

    void setMute (bool) override;
    void setSolo (bool) override;
    void setSoloIsolate (bool) override;

    //==============================================================================
    void setDirtyClips();

    bool canContainPlugin (Plugin*) const override;
    bool willAcceptPlugin (Plugin&);

private:
    friend class Edit;

    juce::ReferenceCountedArray<CollectionClip> collectionClips;
    juce::CachedValue<bool> muted, soloed, soloIsolated;
    bool dirtyClips = true;

    std::mutex pluginMutex;
    juce::ReferenceCountedObjectPtr<VCAPlugin> vcaPlugin;
    juce::ReferenceCountedObjectPtr<VolumeAndPanPlugin> volumePlugin;
    AsyncCaller pluginUpdater;

    void updatePlugins();
    TimeRange getClipExtendedBounds (Clip&);

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
};

}} // namespace tracktion { inline namespace engine
