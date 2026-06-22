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
// One-shot helper that flushes hanging MIDI notes when a MIDI clip is muted.
// Muting a clip mid-note would otherwise leave the synth holding notes on, so this
// asynchronously sends "all notes off" on every channel, then deletes itself.
struct AudioTrack::TrackMuter  : private juce::AsyncUpdater
{
    TrackMuter (AudioTrack& at) : owner (at)        { triggerAsyncUpdate(); }
    ~TrackMuter() override                          { cancelPendingUpdate(); }

    void handleAsyncUpdate() override
    {
        for (int i = 1; i <= 16; ++i)
            owner.injectLiveMidiMessage (juce::MidiMessage::allNotesOff (i), {});

        owner.trackMuter = nullptr;
    }

    AudioTrack& owner;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackMuter)
};

//==============================================================================
// Watches the track state and keeps the individual-freeze flag consistent.
//
// It serves two jobs, both deferred onto the message queue via AsyncUpdater:
//   - triggerFreeze: an explicit async freeze request (freezeTrackAsync) - freeze
//     the track unless it's already group-frozen.
//   - updateFreeze: any state change - if the track claims to be individually
//     frozen but its FreezePointPlugin has gone, drop the frozen flag.
// During Edit load the listener is attached only once loading finishes, so we don't
// react to the flood of property sets that loading produces.
struct AudioTrack::FreezeUpdater : private ValueTreeAllEventListener,
                                   private juce::AsyncUpdater
{
    FreezeUpdater (AudioTrack& at)
        : owner (at)
    {
        if (owner.edit.isLoading())
            loadFinishedCallback = std::make_unique<Edit::LoadFinishedCallback<FreezeUpdater>> (*this, owner.edit);
        else
            state.addListener (this);
    }

    ~FreezeUpdater() override
    {
        state.removeListener (this);
        cancelPendingUpdate();
    }

    void freeze()
    {
        markAndUpdate (triggerFreeze);
    }

    AudioTrack& owner;
    juce::ValueTree state { owner.state };

    /** @internal */
    void editFinishedLoading()
    {
        state.addListener (this);
    }

private:
    std::unique_ptr<Edit::LoadFinishedCallback<FreezeUpdater>> loadFinishedCallback;
    bool triggerFreeze = false, updateFreeze = false;

    void markAndUpdate (bool& flag)     { flag = true; triggerAsyncUpdate(); }

    bool compareAndReset (bool& flag)
    {
        if (! flag)
            return false;

        flag = false;
        return true;
    }

    void valueTreeChanged() override
    {
        markAndUpdate (updateFreeze);
    }

    void handleAsyncUpdate() override
    {
        if (compareAndReset (triggerFreeze))
        {
            if (! owner.isFrozen (groupFreeze))
                owner.setFrozen (true, Track::individualFreeze);
        }

        if (compareAndReset (updateFreeze))
        {
            if (owner.isFrozen (Track::individualFreeze) && (! owner.hasFreezePointPlugin()))
                owner.setFrozen (false, individualFreeze);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreezeUpdater)
};

//==============================================================================
// Sets up the track: binds the CachedValues to their ValueTree properties, then
// (on the message thread, via callBlocking) creates the per-track virtual wave and
// MIDI input devices that recording/monitoring routes through, the TrackOutput that
// routes this track's signal onward, and the FreezeUpdater.
AudioTrack::AudioTrack (Edit& ed, const juce::ValueTree& v)
    : ClipTrack (ed, v, true),
      MacroParameterElement (ed, v)
{
    soloed.referTo (state, IDs::solo, nullptr);
    soloIsolated.referTo (state, IDs::soloIsolate, nullptr);
    muted.referTo (state, IDs::mute, nullptr);
    frozen.referTo (state, IDs::frozen, nullptr);
    frozenIndividually.referTo (state, IDs::frozenIndividually, nullptr);
    ghostTracks.referTo (state, IDs::ghostTracks, nullptr);
    maxInputs.referTo (state, IDs::maxInputs, nullptr, 1);
    compGroup.referTo (state, IDs::compGroup, &edit.getUndoManager(), -1);
    midiNoteMap.referTo (state, IDs::midiNoteMap, nullptr);
    playSlotClips.referTo (state, IDs::playSlotClips, nullptr);

    updateMidiNoteMapCache();

    callBlocking ([this, itemIDString = itemID.toString()]
    {
        WaveDeviceDescription desc;
        desc.name = itemIDString;
        desc.channels = { ChannelIndex (0, juce::AudioChannelSet::left),
                          ChannelIndex (1, juce::AudioChannelSet::right) };

        waveInputDevice = std::make_unique<WaveInputDevice> (edit.engine, TRANS("Track Wave Input"),
                                                             desc, InputDevice::trackWaveDevice);

        midiInputDevice = std::make_unique<VirtualMidiInputDevice> (edit.engine, itemIDString,
                                                                    InputDevice::trackMidiDevice,
                                                                    "TrkMIDI_" + itemIDString,
                                                                    false);

        auto& eid = edit.getEditInputDevices();
        waveInputDevice->setEnabled (eid.isInputDeviceAssigned (*waveInputDevice));
        midiInputDevice->setEnabled (eid.isInputDeviceAssigned (*midiInputDevice));
    });

    output = std::make_unique<TrackOutput> (*this);
    freezeUpdater = std::make_unique<FreezeUpdater> (*this);

    // Reset the MIDI editor's vertical zoom/scroll if the stored values are out of range.
    if (getMidiVerticalOffset() < 0 || getMidiVerticalOffset() > 0.99
         || getMidiVisibleProportion() < 0.1 || getMidiVisibleProportion() > 1.0)
        setVerticalScaleToDefault();

    // Coalesced refresh of auto-crossfades: when a clip start/length changes we
    // re-evaluate overlaps once asynchronously rather than per property change.
    asyncCaller.addFunction (updateAutoCrossfadesFlag,
                             [this]
                             {
                                 for (auto c : getClips())
                                     if (auto acb = dynamic_cast<AudioClipBase*> (c))
                                         if (acb->getAutoCrossfade())
                                             acb->updateAutoCrossfadesAsync (false);
                             });
}

AudioTrack::~AudioTrack()
{
    // If this track's input devices were in use, tear down playback and detach them
    // from every track they were assigned to before this object disappears.
    const bool clearWave = waveInputDevice != nullptr && waveInputDevice->isEnabled();
    const bool clearMidi = midiInputDevice != nullptr && midiInputDevice->isEnabled();

    if (clearWave || clearMidi)
    {
        edit.getTransport().freePlaybackContext();

        for (auto at : getTracksOfType<AudioTrack> (edit, true))
        {
            if (clearWave)  edit.getEditInputDevices().clearInputsOfDevice (*at, *waveInputDevice, &edit.getUndoManager());
            if (clearMidi)  edit.getEditInputDevices().clearInputsOfDevice (*at, *midiInputDevice, &edit.getUndoManager());
        }
    }

    notifyListenersOfDeletion();
}

void AudioTrack::initialise()
{
    CRASH_TRACER

    ClipTrack::initialise();

    // Make sure there's one launcher clip slot per scene.
    if (! edit.isLoading())
        getClipSlotList().ensureNumberOfSlots (edit.getSceneList().getNumScenes());

    // A frozen flag is meaningless without its rendered file - clear it if missing.
    if (frozenIndividually && ! getFreezeFile().existsAsFile())
        setFrozen (false, individualFreeze);

    output->initialise();
}

// Keeps the stored name tidy and propagates it to the input devices. A name that's
// just "Track <n>" is cleared so the track falls back to its dynamic numbered name
// (which stays correct as tracks are reordered).
void AudioTrack::sanityCheckName()
{
    auto n = ClipTrack::getName();

    if ((n.startsWithIgnoreCase ("Track ")
          || n.startsWithIgnoreCase (TRANS("Track") + " "))
         && n.substring (6).trim().containsOnly ("0123456789"))
    {
        // For "Track 1" type names, leave the actual name empty.
        resetName();
        changed();
    }

    // We need to update the alias here as it's the first time the name will be returned correctly upon track creation
    // We also won't get a property change if the track order etc. changes as it will just be empty
    auto devName = getName();

    if (waveInputDevice != nullptr) waveInputDevice->setAlias (devName);
    if (midiInputDevice != nullptr) midiInputDevice->setAlias (devName);
}

// Returns the user-set name, or a generated "Track <n>" if none has been set.
juce::String AudioTrack::getName() const
{
    if (auto n = ClipTrack::getName(); ! n.isEmpty())
        return n;

    return getNameAsTrackNumber();
}

// The track's 1-based position among audio tracks (counting recursively through
// folders), used for the default "Track N" name.
int AudioTrack::getAudioTrackNumber() const noexcept
{
    int result = 1;

    edit.visitAllTracksRecursive ([&] (Track& t)
    {
        if (this == &t)
            return false;

        if (t.isAudioTrack())
            ++result;

        return true;
    });

    return result;
}

juce::String AudioTrack::getNameAsTrackNumber() const
{
    return TRANS("Track") + " " + juce::String (getAudioTrackNumber());
}

juce::String AudioTrack::getNameAsTrackNumberWithDescription() const
{
    auto desc = getNameAsTrackNumber();

    if (auto n = getName(); ! n.startsWithIgnoreCase (TRANS("Track") + " "))
        desc << " (" << n << ")";

    return desc;
}

juce::String AudioTrack::getSelectableDescription()
{
    return getNameAsTrackNumberWithDescription();
}

// Convenience lookups for the standard built-in plugins the engine puts on a track
// (returns the last of each type, i.e. the one nearest the output).
VolumeAndPanPlugin* AudioTrack::getVolumePlugin()     { return pluginList.getPluginsOfType<VolumeAndPanPlugin>().getLast(); }
LevelMeterPlugin* AudioTrack::getLevelMeterPlugin()   { return pluginList.getPluginsOfType<LevelMeterPlugin>().getLast(); }
EqualiserPlugin* AudioTrack::getEqualiserPlugin()     { return pluginList.getPluginsOfType<EqualiserPlugin>().getLast(); }

// Finds an aux-send plugin, selected either by its bus number or by its ordinal
// position among the aux sends on this track.
AuxSendPlugin* AudioTrack::getAuxSendPlugin (int bus, AuxPosition ap) const
{
    if (ap == AuxPosition::byBus)
    {
        for (auto p : pluginList)
            if (auto f = dynamic_cast<AuxSendPlugin*> (p))
                if (bus < 0 || bus == f->getBusNumber())
                    return f;
    }
    else if (ap == AuxPosition::byPosition)
    {
        jassert(bus >= 0);

        int idx = 0;
        for (auto p : pluginList)
        {
            if (auto f = dynamic_cast<AuxSendPlugin*> (p))
            {
                if (idx == bus)
                    return f;

                idx++;
            }
        }
    }

    return {};
}

//==============================================================================
// Resolves a display name for a MIDI note. Search order: this track's custom note
// map, then its plugins, then the destination track, then the master plugins, then
// the MIDI output device, finally falling back to standard note/drum names.
juce::String AudioTrack::getNameForMidiNoteNumber (int note, int midiChannel, bool preferSharp) const
{
    jassert (midiChannel > 0);

    if (midiNoteMapCache.size() > 0)
    {
        auto itr = midiNoteMapCache.find (note);

        if (itr != midiNoteMapCache.end())
            return itr->second;

        return {};
    }

    juce::String s;

    for (auto af : pluginList)
        if (af->hasNameForMidiNoteNumber (note, midiChannel, s))
            return s;

    if (auto dest = output->getDestinationTrack())
        return dest->getNameForMidiNoteNumber (note, midiChannel, preferSharp);

    // try the master plugins..
    for (auto af : edit.getMasterPluginList())
        if (af->hasNameForMidiNoteNumber (note, midiChannel, s))
            return s;

    if (auto mo = dynamic_cast<MidiOutputDevice*> (getOutput().getOutputDevice (true)))
        return mo->getNameForMidiNoteNumber (note, midiChannel, preferSharp);

    return midiChannel == 10 ? TRANS(juce::MidiMessage::getRhythmInstrumentName (note))
                             : juce::MidiMessage::getMidiNoteName (note, preferSharp, true,
                                                                   edit.engine.getEngineBehaviour().getMiddleCOctave());
}

// Parses the user-supplied note-name map (one "78 Some name" entry per line, '//'
// for comments) into a fast int->name lookup used by getNameForMidiNoteNumber.
void AudioTrack::updateMidiNoteMapCache()
{
    midiNoteMapCache.clear();

    auto m = midiNoteMap.get();

    auto lines = juce::StringArray::fromLines (m);
    for (auto l : lines)
    {
        if (l.startsWith ("//"))
            continue;

        int digits = 0;
        for (int i = 0; i < l.length(); i++)
        {
            auto c = l[i];

            if (juce::CharacterFunctions::isDigit (c))
                digits++;
            else
                break;
        }

        if (digits > 0)
            midiNoteMapCache[l.substring (0, digits).getIntValue()] = l.substring (digits).trim();
    }
}

// The following bank/program-name lookups follow the same fallback chain as
// getNameForMidiNoteNumber: this track's plugins -> destination track -> master
// plugins -> MIDI output device -> a sensible default.
bool AudioTrack::areMidiPatchesZeroBased() const
{
    // do something for plugins here
    if (auto dest = output->getDestinationTrack())
        return dest->areMidiPatchesZeroBased();

    // try the master plugins..
    if (auto midiDevice = dynamic_cast<MidiOutputDevice*> (output->getOutputDevice (false)))
        return midiDevice->areMidiPatchesZeroBased();

    return false;
}

juce::String AudioTrack::getNameForBank (int bank) const
{
    juce::String s;

    for (auto p : pluginList)
        if (p->hasNameForMidiBank (bank, s))
            return s;

    if (auto dest = output->getDestinationTrack())
        return dest->getNameForBank (bank);

    // try the master plugins..
    for (auto p : edit.getMasterPluginList())
        if (p->hasNameForMidiBank (bank, s))
            return s;

    if (auto midiDevice = dynamic_cast<MidiOutputDevice*> (output->getOutputDevice (false)))
        return midiDevice->getBankName (bank);

    return edit.engine.getMidiProgramManager().getBankName (0, bank);
}

int AudioTrack::getIdForBank (int bank) const
{
    if (auto dest = output->getDestinationTrack())
        return dest->getIdForBank (bank);

    if (auto midiDevice = dynamic_cast<MidiOutputDevice*> (output->getOutputDevice (false)))
        return midiDevice->getBankID (bank);

    return bank;
}

juce::String AudioTrack::getNameForProgramNumber (int programNumber, int bank) const
{
    juce::String s;

    for (auto p : pluginList)
        if (p->hasNameForMidiProgram (programNumber, bank, s))
            return s;

    if (const AudioTrack* const dest = output->getDestinationTrack())
        return dest->getNameForProgramNumber (programNumber, bank);

    // try the master plugins..
    for (auto p : edit.getMasterPluginList())
        if (p->hasNameForMidiProgram (programNumber, bank, s))
            return s;

    if (auto midiDevice = dynamic_cast<MidiOutputDevice*> (output->getOutputDevice (false)))
        return midiDevice->getProgramName (programNumber, bank);

    return TRANS(juce::MidiMessage::getGMInstrumentName (programNumber));
}

//==============================================================================
// Mute/solo state. The setters just write the flag; the actual audibility decision
// is made by the Edit (which weighs every track's solo/mute) and applied via
// Track::updateAudibility.
void AudioTrack::setMute (bool b)           { muted = b; }
void AudioTrack::setSolo (bool b)           { soloed = b; }
void AudioTrack::setSoloIsolate (bool b)    { soloIsolated = b; }

// Muted if explicitly muted, or - when includeMutingByDestination is set -
// implicitly muted because a folder/destination track it feeds is muted.
bool AudioTrack::isMuted (bool includeMutingByDestination) const
{
    if (muted)
        return true;

    if (includeMutingByDestination)
    {
        if (auto p = getParentFolderTrack())
            return p->isMuted (true);

        if (auto dest = output->getDestinationTrack())
            return dest->isMuted (true);
    }

    return false;
}

// Soloed if explicitly soloed, or - with includeIndirectSolo - implicitly soloed
// because a parent folder or (unless part of a submix) a destination track is soloed.
bool AudioTrack::isSolo (bool includeIndirectSolo) const
{
    if (soloed)
        return true;

    if (includeIndirectSolo)
    {
        // If any of the parent tracks are soloed, this needs to be indirectly soloed
        for (auto p = getParentFolderTrack(); p != nullptr; p = p->getParentFolderTrack())
            if (p->isSolo (false))
                return true;

        if (! isPartOfSubmix())
            if (auto dest = output->getDestinationTrack())
                return dest->isSolo (true);
    }

    return false;
}

bool AudioTrack::isSoloIsolate (bool includeIndirectSolo) const
{
    if (soloIsolated)
        return true;

    if (includeIndirectSolo)
    {
        // If any of the parent tracks are solo isolate, this needs to be indirectly solo isolate
        for (auto p = getParentFolderTrack(); p != nullptr; p = p->getParentFolderTrack())
            if (p->isSoloIsolate (false))
                return true;

        if (! isPartOfSubmix())
            if (auto dest = output->getDestinationTrack())
                return dest->isSoloIsolate (true);
    }

    return false;
}

static bool isInputTrackSolo (const Track& track)
{
    for (auto t : track.getInputTracks())
        if (t->isSolo (true))
            return true;

    return false;
}

// A track must stay audible if one of its input/source tracks is soloed (otherwise
// the soloed source would be silenced by this track being muted), even when the
// normal solo logic would hide it.
bool AudioTrack::isTrackAudible (bool areAnyTracksSolo) const
{
    if (areAnyTracksSolo && isInputTrackSolo (*this))
        return true;

    return Track::isTrackAudible (areAnyTracksSolo);
}

//==============================================================================
// Returns a user-facing warning if the arrangement clips on this track can't be
// heard given its routing/plugins (MIDI with no synth/MIDI out, or wave blocked by
// the output or a plugin that doesn't pass audio), or "" if everything's fine.
juce::String AudioTrack::getTrackPlayabilityWarning() const
{
    bool hasMidi = false, hasWave = false;

    for (auto c : getClips())
    {
        auto type = c->type;

        if (! hasMidi && (type == TrackItem::Type::midi || type == TrackItem::Type::step))
            hasMidi = true;

        if (! hasWave && type == TrackItem::Type::wave)
            hasWave = true;

        if (hasMidi && hasWave)
            break;
    }

    if (hasMidi && ! canPlayMidi())
        return TRANS("This track contains MIDI-generating clips which may be inaudible as it doesn't output to a MIDI device or a plugin synthesiser.")
                  + "\n\n" + TRANS("To change a track's destination, select the track and use its destination list.");

    if (hasWave && ! canPlayAudio())
    {
        if (! getOutput().canPlayAudio())
            return TRANS("This track contains wave clips which may be inaudible as it doesn't output to an audio device.")
                        + "\n\n" + TRANS("To change a track's destination, select the track and use its destination list.");

        return TRANS("This track contains wave clips which may be inaudible as the audio will be blocked by some of the track's plugins.");
    }

    return {};
}

// As getTrackPlayabilityWarning, but inspects the launcher clip slots instead of
// the arrangement clips.
juce::String AudioTrack::getLauncherPlayabilityWarning() const
{
    bool hasMidi = false, hasWave = false;

    if (clipSlotList)
    {
        for (auto slot : clipSlotList->getClipSlots())
        {
            if (slot)
            {
                if (auto c = slot->getClip())
                {
                    auto type = c->type;

                    if (! hasMidi && (type == TrackItem::Type::midi || type == TrackItem::Type::step))
                        hasMidi = true;

                    if (! hasWave && type == TrackItem::Type::wave)
                        hasWave = true;

                    if (hasMidi && hasWave)
                        break;
                }
            }
        }
    }

    if (hasMidi && ! canPlayMidi())
        return TRANS("This track contains MIDI-generating clips which may be inaudible as it doesn't output to a MIDI device or a plugin synthesiser.")
                  + "\n\n" + TRANS("To change a track's destination, select the track and use its destination list.");

    if (hasWave && ! canPlayAudio())
    {
        if (! getOutput().canPlayAudio())
            return TRANS("This track contains wave clips which may be inaudible as it doesn't output to an audio device.")
                        + "\n\n" + TRANS("To change a track's destination, select the track and use its destination list.");

        return TRANS("This track contains wave clips which may be inaudible as the audio will be blocked by some of the track's plugins.");
    }

    return {};
}

// Audio can reach the output only if the output accepts audio and every plugin in
// the chain passes audio through (a synth that takes only MIDI in would block it).
bool AudioTrack::canPlayAudio() const
{
    if (! getOutput().canPlayAudio())
        return false;

    for (auto p : pluginList)
        if (! p->takesAudioInput())
            return false;

    return true;
}

// MIDI is playable if the output is a MIDI device, or a plugin (here or on a parent
// submix) accepts MIDI and turns it into audio that can reach an audio output.
bool AudioTrack::canPlayMidi() const
{
    if (getOutput().canPlayMidi())
        return true;

    for (auto p : pluginList)
        if (p->takesMidiInput())
            return getOutput().canPlayAudio();

    if (isPartOfSubmix())
        for (auto ft = getParentFolderTrack(); ft != nullptr; ft = ft->getParentFolderTrack())
            for (auto p : ft->pluginList)
                if (p->takesMidiInput())
                    return getOutput().canPlayAudio();

    return false;
}

//==============================================================================
// Lazily creates the launcher clip-slot list (and its backing CLIPSLOTS child) on
// first access.
ClipSlotList& AudioTrack::getClipSlotList()
{
    if (! clipSlotList)
        clipSlotList = std::make_unique<ClipSlotList> (state.getOrCreateChildWithName (IDs::CLIPSLOTS, &edit.getUndoManager()), *this);

    return *clipSlotList;
}

//==============================================================================
// Vertical zoom/scroll state for the inline MIDI note editor: midiVProp is the
// fraction of the 128-note range that's visible, midiVOffset is the scroll position.
double AudioTrack::getMidiVerticalOffset() const
{
    return state.getProperty (IDs::midiVOffset, juce::var (defaultMidiVerticalOffset));
}

double AudioTrack::getMidiVisibleProportion() const
{
    return state.getProperty (IDs::midiVProp, juce::var (defaultMidiVisibleProportion));
}

void AudioTrack::setMidiVerticalPos (double visibleProp, double offset)
{
    visibleProp = juce::jlimit (0.0, 1.0, visibleProp);
    auto vo = juce::jlimit (0.0, 1.0 - visibleProp, offset);
    auto vp = juce::jlimit (0.1, 1.0 - vo, visibleProp);

    state.setProperty (IDs::midiVOffset, vo, nullptr);
    state.setProperty (IDs::midiVProp,   vp, nullptr);
}

void AudioTrack::setVerticalScaleToDefault()
{
    auto midiNotes = juce::jlimit (1, 128, 12 * static_cast<int> (edit.engine.getPropertyStorage()
                                                                   .getProperty (SettingID::midiEditorOctaves, 3)));

    defaultMidiVisibleProportion = midiNotes / 128.0;
    defaultMidiVerticalOffset = (1.0 - defaultMidiVisibleProportion) * 0.5;

    state.removeProperty (IDs::midiVOffset, nullptr);
    state.removeProperty (IDs::midiVProp, nullptr);
}

// "Ghost" tracks are other tracks whose notes are drawn faintly behind this track's
// MIDI editor for reference. Stored as a list of track IDs.
void AudioTrack::setTrackToGhost (AudioTrack* track, bool shouldGhost)
{
    if (track == nullptr)
        return;

    auto list = EditItemID::parseStringList (ghostTracks.get());
    bool isGhosted = list.contains (track->itemID);

    if (isGhosted != shouldGhost)
    {
        if (shouldGhost)
            list.add (track->itemID);
        else
            list.removeAllInstancesOf (track->itemID);

        ghostTracks = EditItemID::listToString (list);
    }
}

juce::Array<AudioTrack*> AudioTrack::getGhostTracks() const
{
    if (ghostTracks.get().isEmpty())
        return {};

    juce::Array<AudioTrack*> tracks;

    for (auto& trackID : EditItemID::parseStringList (ghostTracks))
        if (auto at = findAudioTrackForID (edit, trackID))
            tracks.add (at);

    return tracks;
}

// "Guide notes" are the audible preview notes played when the user clicks/drags in
// the MIDI editor or piano keyboard. They're injected straight into the live MIDI
// stream (not recorded). currentlyPlayingGuideNotes tracks held notes so they can be
// turned off again; autorelease schedules an automatic note-off via the Timer.
void AudioTrack::playGuideNote (int note, MidiChannel midiChannel, int velocity, bool stopOtherFirst, bool forceNote, bool autorelease)
{
    jassert (midiChannel.isValid()); //SysEx?
    jassert (velocity >= 0 && velocity <= 127);

    if (stopOtherFirst)
        turnOffGuideNotes (midiChannel);

    if (note >= 0 && (forceNote || edit.engine.getEngineBehaviour().shouldPlayMidiGuideNotes()))
    {
        const int pitch = juce::jlimit (0, 127, note);

        if (! currentlyPlayingGuideNotes.contains (pitch))
        {
            currentlyPlayingGuideNotes.add (pitch);
            injectLiveMidiMessage (juce::MidiMessage::noteOn (midiChannel.getChannelNumber(),
                                                              pitch, (uint8_t) velocity),
                                   {});
        }

        if (autorelease)
            startTimer (100);
    }
}

void AudioTrack::playGuideNotes (const juce::Array<int>& notes, MidiChannel midiChannel,
                                 const juce::Array<int>& vels, bool stopOthersFirst)
{
    jassert (midiChannel.isValid()); //SysEx?

    if (stopOthersFirst)
        turnOffGuideNotes (midiChannel);

    if (notes.size() < 8 && edit.engine.getEngineBehaviour().shouldPlayMidiGuideNotes())
    {
        for (int i = 0; i < notes.size(); ++i)
        {
            const int pitch = juce::jlimit (0, 127, notes.getUnchecked (i));

            if (! currentlyPlayingGuideNotes.contains (pitch))
            {
                currentlyPlayingGuideNotes.add (pitch);
                injectLiveMidiMessage (juce::MidiMessage::noteOn (midiChannel.getChannelNumber(),
                                                                  pitch, (uint8_t) vels.getUnchecked (i)),
                                       {});
            }
        }
    }
}

// Stops all guide notes on every channel (called e.g. on the autorelease timer or
// when starting a new preview).
void AudioTrack::turnOffGuideNotes()
{
    stopTimer();

    for (int ch = 1; ch <= 16; ch++)
        turnOffGuideNotes (MidiChannel (ch));
}

void AudioTrack::turnOffGuideNotes (MidiChannel midiChannel)
{
    jassert (midiChannel.isValid()); //SysEx?
    auto channel = midiChannel.getChannelNumber();

    for (auto note : currentlyPlayingGuideNotes)
        injectLiveMidiMessage (juce::MidiMessage::noteOff (channel, note), {});

    currentlyPlayingGuideNotes.clear();
}

//==============================================================================
// Listeners receive live/recorded MIDI generated by this track. Adding the first
// listener restarts playback so the graph is rebuilt with a node that forwards MIDI
// to listeners.
void AudioTrack::addListener (Listener* l)
{
    if (listeners.isEmpty())
        edit.restartPlayback();

    listeners.add (l);
}

void AudioTrack::removeListener (Listener* l)
{
    listeners.remove (l);
    // N.B. Don't call restartPlayback here or it will be impossible to clear the audio graph
}

//==============================================================================
// Reacts to property changes on the track's own state or on its clips. Track-level
// changes update cached values and trigger the appropriate side effects (clearing
// inputs, freezing/unfreezing, renaming devices...); clip-level changes drive
// auto-crossfade refreshes and the mute-time all-notes-off flush.
void AudioTrack::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& i)
{
    if (v == state)
    {
        if (i == IDs::maxInputs)
        {
            maxInputs.forceUpdateOfCachedValue();
            edit.getEditInputDevices().clearAllInputs (*this, &edit.getUndoManager());
        }
        else if (i == IDs::ghostTracks)
        {
            if (ghostTracks.get().isEmpty())
                ghostTracks.resetToDefault();

            changed();
        }
        else if (i == IDs::playSlotClips)
        {
            // Switching away from launcher playback stops any currently-playing slots.
            playSlotClips.forceUpdateOfCachedValue();

            if (! playSlotClips.get())
                for (auto cs : getClipSlotList().getClipSlots())
                    if (auto c = cs->getClip())
                        if (auto lh = c->getLaunchHandle(); lh->getPlayingStatus() == LaunchHandle::PlayState::playing)
                            lh->stop ({});
        }
        else if (i == IDs::compGroup)
        {
            changed();
        }
        else if (i == IDs::frozen)
        {
            frozen.forceUpdateOfCachedValue();
            changed();
        }
        else if (i == IDs::frozenIndividually)
        {
            frozenIndividually.forceUpdateOfCachedValue();

            if (frozenIndividually)
                freezeTrack();
            else
                unFreezeTrack();
        }
        else if (i == IDs::name)
        {
            auto devName = getName();

            waveInputDevice->setAlias (devName);
            midiInputDevice->setAlias (devName);
        }
        else if (i == IDs::midiNoteMap)
        {
            updateMidiNoteMapCache();
        }
    }
    else if (Clip::isClipState (v))
    {
        TRACKTION_ASSERT_MESSAGE_THREAD;

        // A clip moved/resized: re-evaluate auto-crossfades with neighbours.
        if (i == IDs::start || i == IDs::length)
            asyncCaller.updateAsync (updateAutoCrossfadesFlag);

        // A MIDI clip was just muted: flush any hanging notes (unless it's set to
        // keep processing while muted).
        if (i == IDs::mute && bool (v.getProperty (i)))
            if (trackMuter == nullptr && ! bool (v.getProperty (IDs::processMidiWhenMuted, false)))
                if (v.hasType (IDs::MIDICLIP))
                    trackMuter = std::make_unique<TrackMuter> (*this);
    }

    ClipTrack::valueTreePropertyChanged (v, i);
}

// When the track is (re)attached to the Edit, make sure the global scene count is at
// least as large as this track's number of clip slots.
void AudioTrack::valueTreeParentChanged (juce::ValueTree& v)
{
    ClipTrack::valueTreeParentChanged (v);

    if (state.getParent().isValid())
        if (int numScenes = getClipSlotList().getClipSlots().size(); numScenes > 0)
            edit.getSceneList().ensureNumberOfScenes (getClipSlotList().getClipSlots().size());
}

//==============================================================================
// True if an input device is armed/recording onto this track.
bool AudioTrack::hasAnyLiveInputs()
{
    for (auto in : edit.getAllInputDevices())
        if (in->isRecordingActive (itemID) && in->getTargets().contains (itemID))
            return true;

    return false;
}

// True if another track routes its output into this one (i.e. this is a destination).
bool AudioTrack::hasAnyTracksFeedingIn()
{
    for (auto t : getAudioTracks (edit))
        if (t != this && t->getOutput().feedsInto (this))
            return true;

    return false;
}

//==============================================================================
// Pushes a live MIDI message (soft keyboard, guide note, controller...) into the
// playback graph by offering it to the listeners. If nothing consumed it, warns the
// user that the message had nowhere to go.
void AudioTrack::injectLiveMidiMessage (const MidiMessageWithSource& message)
{
    TRACKTION_ASSERT_MESSAGE_THREAD
    bool wasUsed = false;
    listeners.call (&Listener::injectLiveMidiMessage, *this, message, wasUsed);

    if (! wasUsed)
        edit.warnOfWastedMidiMessages (nullptr, this);
}

void AudioTrack::injectLiveMidiMessage (const juce::MidiMessage& m, MPESourceID source)
{
    injectLiveMidiMessage ({ m, source });
}

// Merges a recorded/imported MIDI sequence into an existing clip on the track. If no
// target clip is given, picks the first MIDI clip overlapping the sequence's time
// range. Returns false if there's no suitable clip to merge into.
bool AudioTrack::mergeInMidiSequence (juce::MidiMessageSequence ms, TimePosition startTime,
                                      MidiClip* mc, MidiList::NoteAutomationType automationType)
{

    if (mc == nullptr)
    {
        const auto start = TimePosition::fromSeconds (ms.getStartTime()) + toDuration (startTime);
        const auto end = TimePosition::fromSeconds (ms.getEndTime()) + toDuration (startTime);

        for (auto c : getClips())
        {
            if (c->getPosition().time.overlaps ({ start, end }))
            {
                mc = dynamic_cast<MidiClip*> (c);

                if (mc != nullptr)
                    break;
            }
        }
    }

    if (mc != nullptr)
    {
        tracktion::mergeInMidiSequence (*mc, std::move (ms), toDuration (startTime), automationType);
        return true;
    }

    return false;
}

// Returns the tracks (audio tracks and submix folders) that route their output into
// this one, excluding tracks that are part of a submix (those feed their folder, not
// this track directly).
juce::Array<Track*> AudioTrack::getInputTracks() const
{
    juce::Array<Track*> inputTracks;

    for (auto track : getAudioTracks (edit))
        if (! track->isPartOfSubmix() && track != this && track->getOutput().feedsInto (this))
            inputTracks.add (track);

    for (auto track : getTracksOfType<FolderTrack> (edit, true))
        if (! track->isPartOfSubmix() && track->getOutput() != nullptr && track->getOutput()->feedsInto (this))
            inputTracks.add (track);

    return inputTracks;
}

static bool canTrackBeChanged (InputDeviceInstance* idi)
{
    if (idi->isRecording())
    {
        idi->edit.engine.getUIBehaviour().showWarningMessage (TRANS("Can't change tracks whilst recording is active"));
        return false;
    }

    return true;
}

// Sets how many simultaneous inputs the track accepts, but refuses to change it
// while any of its inputs is actively recording.
void AudioTrack::setMaxNumOfInputs (int n)
{
    for (auto* idi : edit.getEditInputDevices().getDevicesForTargetTrack (*this))
        if (! canTrackBeChanged (idi))
            return;

    maxInputs = n;
}

//==============================================================================
// Freezing renders the track (and its inputs) to an audio file and plays that back
// instead of processing the live graph, to save CPU. There are two kinds: groupFreeze
// (several tracks bounced together, tracked by the 'frozen' flag) and individualFreeze
// (this track alone, tracked by 'frozenIndividually').
bool AudioTrack::isFrozen (FreezeType t) const
{
    return t == anyFreeze ? (frozen || frozenIndividually)
                          : (t == groupFreeze ? frozen : frozenIndividually);
}

// Sets the requested freeze flag, refusing to freeze a track that outputs into
// another track or a submix (you should freeze the destination instead, since this
// track's audio isn't independently routable to the render). Setting the flag is
// what kicks off the actual freeze/unfreeze via valueTreePropertyChanged.
void AudioTrack::setFrozen (bool b, FreezeType type)
{
    if (type == individualFreeze)
    {
        if (frozenIndividually != b)
        {
            if (b && getOutput().getDestinationTrack() != nullptr)
            {
                edit.engine.getUIBehaviour().showWarningMessage (TRANS("Tracks which output to another track can't themselves be frozen; "
                                                                       "instead, you should freeze the track they input into."));
            }
            else
            {
                frozenIndividually = b;
            }
        }
    }
    else
    {
        if (frozen != b)
        {
            if (! edit.isLoading())
            {
                const auto outputsToSubmixTrack = [this]
                {
                    if (auto folder = getParentFolderTrack())
                        return folder->isSubmixFolder();

                    return false;
                };

                if (b && (getOutput().getDestinationTrack() != nullptr || outputsToSubmixTrack()))
                {
                    edit.engine.getUIBehaviour().showWarningMessage (TRANS("Tracks which output to another track can't themselves be frozen; "
                                                                           "instead, you should freeze the track they input into."));
                }
                else
                {
                    frozen = b;
                }
            }
        }
    }
}

// Audio tracks accept any plugin except a VCA (that's a folder-track concept), and
// allow at most one FreezePointPlugin.
bool AudioTrack::canContainPlugin (Plugin* p) const
{
    const bool isFreezePoint = dynamic_cast<FreezePointPlugin*> (p) != nullptr;

    return dynamic_cast<VCAPlugin*> (p) == nullptr
            && (! isFreezePoint
                 || (isFreezePoint && (p->getOwnerTrack() == this || ! hasFreezePointPlugin())));
}

//==============================================================================
// While one of these is alive, unFreezeTrack won't delete the freeze-point plugin -
// used to keep the freeze point in place across an operation that briefly unfreezes.
AudioTrack::FreezePointRemovalInhibitor::FreezePointRemovalInhibitor (AudioTrack& at) : track (at)  { ++track.freezePointRemovalInhibitor; }
AudioTrack::FreezePointRemovalInhibitor::~FreezePointRemovalInhibitor()                             { --track.freezePointRemovalInhibitor; }

// Performs an individual freeze: renders everything up to the freeze point (this
// track plus any input tracks) to the freeze file, then marks those plugins frozen
// so the live graph bypasses them and plays the rendered file instead. Mute/solo are
// temporarily neutralised so the render captures the track in isolation.
void AudioTrack::freezeTrack()
{
    insertFreezePointIfRequired();
    const FreezePointPlugin::ScopedPluginDisabler spd (*this, juce::Range<int> (getIndexOfFreezePoint(),
                                                                                pluginList.size()));

    auto& dm = edit.engine.getDeviceManager();

    const bool shouldBeMuted = isMuted (true);
    setMute (false);
    const FreezePointPlugin::ScopedTrackUnsoloer stu (edit);

    juce::BigInteger trackNum;
    trackNum.setBit (getIndexInEditTrackList());
    auto freezeFile = getFreezeFile();
    freezeFile.deleteFile();

    juce::Array<EditItemID> trackIDs { itemID };
    juce::Array<Clip*> clips (getClips());

    for (auto inputTrack : getInputTracks())
    {
        trackIDs.addIfNotAlreadyThere (inputTrack->itemID);

        if (auto ct = dynamic_cast<ClipTrack*> (inputTrack))
            clips.addArray (ct->getClips());
    }

    Renderer::Parameters r (edit);
    r.tracksToDo = trackNum;
    r.destFile = freezeFile;
    r.audioFormat = edit.engine.getAudioFileFormatManager().getFrozenFileFormat();
    r.blockSizeForAudio = dm.getBlockSize();
    r.sampleRateForAudio = dm.getSampleRate();
    r.time = { {}, getLengthIncludingInputTracks() };
    r.endAllowance = RenderOptions::findEndAllowance (edit, &trackIDs, nullptr);
    r.canRenderInMono = true;
    r.mustRenderInMono = false;
    r.usePlugins = true;
    r.useMasterPlugins = false;

    const Edit::ScopedRenderStatus srs (edit, true);
    const auto desc = TRANS("Creating track freeze for \"XDVX\"")
                        .replace ("XDVX", getName()) + "...";

    if (getProjectForEdit (edit) != nullptr)
        Renderer::renderToProjectItem (desc, r, ProjectItem::Category::frozen);
    else
        Renderer::renderToFile (desc, r);

    freezePlugins (juce::Range<int> (0, getIndexOfFreezePoint()));
    setMute (shouldBeMuted);

    if (! r.destFile.existsAsFile())
    {
        edit.engine.getUIBehaviour().showWarningMessage (TRANS("Nothing to freeze"));
        setFrozen (false, individualFreeze);
        return;
    }

    changed();
}

// Index of the FreezePointPlugin in the plugin chain (the boundary between frozen
// and live plugins), or -1 if there isn't one.
int AudioTrack::getIndexOfFreezePoint()
{
    int i = 0;

    for (auto p : pluginList)
    {
        if (dynamic_cast<FreezePointPlugin*> (p) != nullptr)
            return i;

        ++i;
    }

    return -1;
}

// Places the freeze point immediately after a chosen plugin (replacing any existing
// one), so everything up to and including that plugin gets baked when frozen.
void AudioTrack::insertFreezePointAfterPlugin (const Plugin::Ptr& p)
{
    auto& pl = pluginList;

    if (! pl.getPlugins().contains (p.get()))
        return;

    removeFreezePoint();
    pl.insertPlugin (FreezePointPlugin::create(), pl.getPlugins().indexOf (p.get()) + 1);

    edit.dispatchPendingUpdatesSynchronously();
    // need to force the audio device to update before we start the render
}

// Removes any freeze-point plugin(s) from the chain.
void AudioTrack::removeFreezePoint()
{
    auto& pl = pluginList;

    for (int i = pl.size(); --i >= 0;)
        if (auto f = dynamic_cast<FreezePointPlugin*> (pl[i]))
            f->deleteFromParent();
}

// Ensures a freeze point exists, inserting one at the default position if needed.
// Returns true if it had to add one.
bool AudioTrack::insertFreezePointIfRequired()
{
    if (getIndexOfFreezePoint() != -1)
        return false;

    if (auto p = pluginList.insertPlugin (FreezePointPlugin::create(), getIndexOfDefaultFreezePoint()))
        auto freezer = FreezePointPlugin::createTrackFreezer (p);

    edit.dispatchPendingUpdatesSynchronously();
    // need to force the audio device to update before we start the render

    return true;
}

// Requests a freeze on the message loop (safe to call from anywhere) rather than
// freezing synchronously.
void AudioTrack::freezeTrackAsync() const
{
    freezeUpdater->freeze();
}

// Works out where a new freeze point should go based on the user's freeze-point
// preference (before all plugins, or pre/post the volume/pan fader).
int AudioTrack::getIndexOfDefaultFreezePoint()
{
    int position = edit.engine.getPropertyStorage().getProperty (SettingID::freezePoint, 1);

    if (position == FreezePointPlugin::beforeAllPlugins)
        return 0;

    const auto& pl = pluginList;

    for (int i = 0; i < pl.size(); ++i)
    {
        if (dynamic_cast<VolumeAndPanPlugin*> (pl[i]) != nullptr)
        {
            if (position == FreezePointPlugin::preFader)
                return i;

            if (position == FreezePointPlugin::postFader)
                return i + 1;
        }
    }

    return -1;
}

// Marks the plugins whose indices fall in the range as frozen (bypassed in the live
// graph) and unfreezes the rest.
void AudioTrack::freezePlugins (juce::Range<int> pluginsToFreeze)
{
    int i = 0;

    for (auto p : pluginList)
        p->setFrozen (pluginsToFreeze.contains (i++));
}

// Reverses a freeze: un-bypasses all plugins and removes the freeze point if it's in
// its default spot (unless something is inhibiting its removal).
void AudioTrack::unFreezeTrack()
{
    // Remove the freeze point if it's in the default location as it will be put back there anyway
    int defaultPosition = edit.engine.getPropertyStorage().getProperty (SettingID::freezePoint, 0);
    auto defaultIndex = getIndexOfDefaultFreezePoint();
    auto freezePosition = (defaultPosition == FreezePointPlugin::postFader) ? defaultIndex
                                                                            : std::max (0, defaultIndex - 1);

    if (getIndexOfFreezePoint() == freezePosition)
        if (freezePointRemovalInhibitor == 0)
            if (auto f = dynamic_cast<FreezePointPlugin*> (pluginList[freezePosition]))
                f->deleteFromParent();

    // Unfreeze all plugins
    freezePlugins ({});

    changed();
}

// The temp file the rendered (frozen) audio is written to / played back from.
juce::File AudioTrack::getFreezeFile() const
{
    return TemporaryFileManager::getFreezeFileForTrack (*this);
}

// True if any plugin in the Edit uses this track as its sidechain input - such a
// track must keep processing even when muted so the sidechain still receives signal.
bool AudioTrack::isSidechainSource() const
{
    for (auto p : edit.getPluginCache().getPlugins())
        if (p->getSidechainSourceID() == itemID)
            return true;

    return false;
}

// The reverse lookup: the tracks that feed this track's own plugins' sidechain inputs.
juce::Array<Track*> AudioTrack::findSidechainSourceTracks() const
{
    juce::Array<Track*> srcTracks;

    for (auto p : getAllPlugins())
    {
        auto srcId = p->getSidechainSourceID();

        if (srcId.isValid())
            if (auto srcTrack = findTrackForID (edit, srcId))
                srcTracks.addIfNotAlreadyThere (srcTrack);
    }

    return srcTracks;
}

}} // namespace tracktion { inline namespace engine
