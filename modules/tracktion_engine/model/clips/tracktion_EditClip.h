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
    An audio clip whose source is another Edit — i.e. a whole sub-session nested as a
    single clip ("Edits within Edits").

    EditClip references a source Edit by ProjectItemID and treats that Edit's rendered
    output as its audio material. Because a live Edit can't be streamed cheaply in real
    time, EditClip is always a render-based AudioClipBase: it bounces the source Edit to
    a proxy AudioFile (via RenderManager) and plays that, re-rendering when the source
    changes. It listens to the source through an EditSnapshot so it can invalidate its
    hash and trigger a re-render when the referenced Edit is edited.

    getWaveInfo()/getSourceLength reflect the rendered proxy's format rather than a file
    on disk, which is why the source is described by an EditSnapshot instead of a plain
    AudioFile.

    @see AudioClipBase, EditSnapshot, RenderManager, ProjectItemID
*/
class EditClip    : public AudioClipBase,
                    private EditSnapshot::Listener
{
public:
    //==============================================================================
    EditClip (const juce::ValueTree&, EditItemID, ClipOwner&, ProjectItemID sourceEdit);
    ~EditClip() override;

    using Ptr = juce::ReferenceCountedObjectPtr<EditClip>;

    /** Returns the AudioFileInfo for an edit clip.
        This is a bit of a hack but necessary to get the AudioSegmentList structures
        to work correctly.
    */
    AudioFileInfo getWaveInfo()  override               { return waveInfo; }

    /** Returns the cached EditSnapshot that represents the current state of the source. */
    EditSnapshot::Ptr getEditSnapshot() noexcept        { return editSnapshot; }

    AudioFile getAudioFile() const override;

    //==============================================================================
    void initialise() override;
    void cloneFrom (Clip*) override;

    bool isMidi() const override                        { return false; }
    bool canHaveEffects() const override                { return false; }

    double getCurrentStretchRatio() const;

    //==============================================================================
    juce::String getSelectableDescription() override;

    juce::File getOriginalFile() const override;
    HashCode getHash() const override                   { return hash; }

    void setLoopDefaults() override;

    void setTracksToRender (const juce::Array<EditItemID>& trackIDs);

    //==============================================================================
    bool requiresRenderingSource() const override;
    bool needsRender() const override;
    RenderManager::Job::Ptr getRenderJob (const AudioFile& destFile) override;
    void renderComplete() override;
    juce::String getRenderMessage() override;
    juce::String getClipMessage() override;

    //==============================================================================
    TimeDuration getSourceLength() const override       { return editSnapshot == nullptr ? 0_td : editSnapshot->getLength(); }
    bool usesSourceFile() const override                { return false; }
    void sourceMediaChanged() override;
    void changed() override;

    bool isUsingFile (const AudioFile& af) override;

    RenderOptions& getRenderOptions() const noexcept    { jassert (renderOptions != nullptr); return *renderOptions; }

    ProjectItem::Ptr createUniqueCopy();
    HashCode generateHash();

    juce::CachedValue<bool> copyColourFromMarker, trimToMarker, renderEnabled;

protected:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

private:
    //==============================================================================
    AsyncCaller sourceIdUpdater;

    //==============================================================================
    ProjectItemID lastSourceId;
    EditSnapshot::Ptr editSnapshot;
    juce::ReferenceCountedArray<EditSnapshot> referencedEdits;

    AudioFileInfo waveInfo;
    HashCode hash = 0;
    std::unique_ptr<RenderOptions> renderOptions;
    bool sourceMediaReEntrancyCheck = false;

    //==============================================================================
    void updateWaveInfo();
    void updateReferencedEdits();
    void updateLoopInfoBasedOnSource (bool updateLength);

    void editChanged (EditSnapshot&) override;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditClip)
};

}} // namespace tracktion { inline namespace engine
