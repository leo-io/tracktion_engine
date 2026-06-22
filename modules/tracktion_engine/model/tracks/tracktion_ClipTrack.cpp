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
// Keeps the track's CollectionClips in sync with the clips' group IDs.
//
// CollectionClips aren't stored in the ValueTree - they're derived groupings, one
// per distinct group ID, that let several grouped clips be treated as a single
// TrackItem. This listener watches the track's clip state and rebuilds the groupings
// as clips are added, removed, re-grouped or moved, marking trackItemsDirty so the
// cached track-item list is regenerated lazily on next access.
struct ClipTrack::CollectionClipList  : public juce::ValueTree::Listener
{
    CollectionClipList (ClipTrack& t, juce::ValueTree& v) : ct (t), state (v)
    {
        state.addListener (this);
    }

    ~CollectionClipList() override
    {
        state.removeListener (this);
    }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (id == IDs::groupID)
        {
            // A clip's group membership changed. First detach it from any collection
            // that still holds it (deleting that collection if it's now empty)...
            if (auto c = ct.findClipForID (EditItemID::fromID (v)))
            {
                for (auto cc : collectionClips)
                {
                    if (cc->containsClip (c))
                    {
                        cc->removeClip (c);

                        if (cc->getNumClips() == 0)
                        {
                            collectionClips.removeObject (cc);
                            ct.trackItemsDirty = true;
                            break;
                        }

                        cc->updateStartAndEnd();
                        ct.trackItemsDirty = true;
                    }
                }

                // ...then add it to the collection for its new group, creating one
                // if needed. The clip is deselected so the collection (not the clip)
                // becomes the selectable unit.
                if (c->isGrouped())
                {
                    auto cc = findOrCreateCollectionClip (c->getGroupID());
                    cc->addClip (c);
                    cc->updateStartAndEnd();
                    ct.trackItemsDirty = true;

                    c->deselect();
                }
            }
        }
        else if (id == IDs::start || id == IDs::length)
        {
            // A grouped clip moved or resized, so its collection's overall span may
            // have changed - recompute the collection's start/end.
            if (auto c = ct.findClipForID (EditItemID::fromID (v)))
            {
                if (c->isGrouped())
                {
                    if (auto cc = findCollectionClip (c->getGroupID()))
                    {
                        cc->updateStartAndEnd();
                        ct.trackItemsDirty = true;
                    }
                }
            }
        }
    }

    // A clip was added to the track: if it belongs to a group, slot it into the
    // matching collection.
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override
    {
        if (Clip::isClipState (child))
        {
            if (auto c = ct.findClipForID (EditItemID::fromID (child)))
            {
                if (c->isGrouped())
                {
                    auto cc = findOrCreateCollectionClip (c->getGroupID());
                    cc->addClip (c);
                    cc->updateStartAndEnd();
                    ct.trackItemsDirty = true;
                }
            }
        }
    }

    // A clip was removed from the track: pull it out of whichever collection holds
    // it (by ID, since the Clip object may already be gone) and drop empty collections.
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override
    {
        if (Clip::isClipState (child))
        {
            for (auto cc : collectionClips)
            {
                if (cc->removeClip (EditItemID::fromID (child)))
                {
                    if (cc->getNumClips() == 0)
                    {
                        collectionClips.removeObject (cc);
                        ct.trackItemsDirty = true;
                        break;
                    }

                    cc->updateStartAndEnd();
                    ct.trackItemsDirty = true;
                }
            }
        }
    }

    // Returns the collection for a group ID, creating an empty one if none exists yet.
    CollectionClip* findOrCreateCollectionClip (EditItemID groupID)
    {
        for (auto cc : collectionClips)
            if (cc->getGroupID() == groupID)
                return cc;

        auto cc = new CollectionClip (ct);
        cc->setGroupID (groupID);
        collectionClips.add (cc);
        ct.trackItemsDirty = true;

        return cc;
    }

    // Returns the collection for a group ID, or null if there isn't one.
    CollectionClip* findCollectionClip (EditItemID groupID)
    {
        for (auto cc : collectionClips)
            if (cc->getGroupID() == groupID)
                return cc;

        return {};
    }

    // Called by ClipTrack when a grouped clip is first created (the ValueTree child
    // may already have existed before the listener could see it being added).
    void clipCreated (Clip& c)
    {
        auto cc = findOrCreateCollectionClip (c.getGroupID());
        cc->addClip (&c);
        cc->updateStartAndEnd();
        ct.trackItemsDirty = true;
    }

    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    ClipTrack& ct;
    juce::ValueTree& state;

    juce::ReferenceCountedArray<CollectionClip> collectionClips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CollectionClipList)
};

//==============================================================================
ClipTrack::ClipTrack (Edit& ed, const juce::ValueTree& v, bool hasModifierList)
    : Track (ed, v, hasModifierList)
{
    collectionClipList = std::make_unique<CollectionClipList> (*this, state);
}

ClipTrack::~ClipTrack()
{
    // Release any ARA (plugin-side audio analysis) resources the clips hold before
    // the track and its clips are destroyed.
    for (auto c : getClips())
        if (auto acb = dynamic_cast<AudioClipBase*> (c))
            acb->tearDownARA();
}

void ClipTrack::initialise()
{
    // Wire up the ClipOwner half of this object (which manages the actual Clip
    // instances) before the Track half initialises.
    initialiseClipOwner (edit, state);
    Track::initialise();
}

// Pushes any cached/in-memory state (track + each clip) back into the ValueTree so
// it's up to date before the Edit is saved.
void ClipTrack::flushStateToValueTree()
{
    Track::flushStateToValueTree();

    for (auto c : getClips())
        c->flushStateToValueTree();
}

//==============================================================================
// Rebuilds the cached, time-sorted list of TrackItems (clips + collection clips)
// if it's been marked dirty. This list backs the getNumTrackItems/getTrackItem
// queries; caching it avoids re-sorting on every access while clips are unchanged.
void ClipTrack::refreshTrackItems() const
{
    TRACKTION_ASSERT_MESSAGE_THREAD

    if (trackItemsDirty)
    {
        trackItemsDirty = false;

        trackItems.clear();
        trackItems.ensureStorageAllocated (getClips().size());

        for (auto clip : getClips())
            trackItems.add (clip);

        for (auto cc : collectionClipList->collectionClips)
            trackItems.add (cc);

        TrackItem::sortByTime (trackItems);
    }
}

int ClipTrack::getNumTrackItems() const
{
    refreshTrackItems();
    return trackItems.size();
}

TrackItem* ClipTrack::getTrackItem (int idx) const
{
    refreshTrackItems();
    return trackItems[idx];
}

int ClipTrack::indexOfTrackItem (TrackItem* ti) const
{
    refreshTrackItems();
    return trackItems.indexOf (ti);
}

int ClipTrack::getIndexOfNextTrackItemAt (TimePosition time)
{
    refreshTrackItems();
    return findIndexOfNextItemAt (trackItems, time);
}

TrackItem* ClipTrack::getNextTrackItemAt (TimePosition time)
{
    refreshTrackItems();
    return trackItems[getIndexOfNextTrackItemAt (time)];
}

//==============================================================================
CollectionClip* ClipTrack::getCollectionClip (int index)  const noexcept
{
    return collectionClipList->collectionClips[index].get();
}

CollectionClip* ClipTrack::getCollectionClip (Clip* clip) const
{
    if (clip->isGrouped())
        for (auto cc : collectionClipList->collectionClips)
            if (cc->containsClip (clip))
                return cc;

    return {};
}

int ClipTrack::getNumCollectionClips() const noexcept
{
    return collectionClipList->collectionClips.size();
}

int ClipTrack::indexOfCollectionClip (CollectionClip* cc) const
{
    return collectionClipList->collectionClips.indexOf (cc);
}

int ClipTrack::getIndexOfNextCollectionClipAt (TimePosition time)
{
    return findIndexOfNextItemAt (collectionClipList->collectionClips, time);
}

CollectionClip* ClipTrack::getNextCollectionClipAt (TimePosition time)
{
    return collectionClipList->collectionClips [getIndexOfNextCollectionClipAt (time)].get();
}

bool ClipTrack::contains (CollectionClip* cc) const
{
    return collectionClipList->collectionClips.contains (cc);
}

//==============================================================================
// Finds a clip on this track by ID, searching both the arrangement clips and (for
// audio tracks) the launcher clip slots.
Clip* ClipTrack::findClipForID (EditItemID id) const
{
    for (auto c : getClips())
        if (c->itemID == id)
            return c;

    if (auto at = dynamic_cast<const AudioTrack*> (this))
        for (auto slot : const_cast<AudioTrack*> (at)->getClipSlotList().getClipSlots())
            if (auto c = slot->getClip())
                if (c->itemID == id)
                    return c;

    return {};
}

TimeDuration ClipTrack::getLength() const
{
    return toDuration (getTotalRange().getEnd());
}

// The track's length extended to cover any tracks that feed into it (e.g. a
// submix/aux source), recursing through the input chain so the longest contributor
// determines the result.
TimeDuration ClipTrack::getLengthIncludingInputTracks() const
{
    auto l = getLength();

    for (auto t : getAudioTracks (edit))
        if (t != this && t->getOutput().getDestinationTrack() == this)
            l = std::max (l, t->getLengthIncludingInputTracks());

    return l;
}

// The span from the earliest clip start to the latest clip end on this track.
TimeRange ClipTrack::getTotalRange() const
{
    return findUnionOfEditTimeRanges (getClips());
}

// Re-parents an existing clip onto this track (used when moving a clip between
// tracks), enforcing the per-track clip limit. Reparenting the clip's ValueTree is
// what actually moves it; the ClipOwner machinery picks up the change.
bool ClipTrack::addClip (const Clip::Ptr& clip)
{
    CRASH_TRACER

    if (clip != nullptr)
    {
        if (getClips().size() < edit.engine.getEngineBehaviour().getEditLimits().maxClipsInTrack)
        {
            jassert (findClipForID (clip->itemID) == nullptr);

            auto um = clip->getUndoManager();
            clip->state.getParent().removeChild (clip->state, um);
            state.addChild (clip->state, -1, um);

            changed();
            return true;
        }
        else
        {
            clip->edit.engine.getUIBehaviour().showWarningMessage (TRANS("Can't add any more clips to this track!"));
        }
    }
    return false;
}

// Adds an externally-built CollectionClip, replacing any auto-generated collection
// that already contains its clips so we don't end up with duplicates.
void ClipTrack::addCollectionClip (CollectionClip* cc)
{
    CollectionClip::Ptr refHolder (cc);

    // if this collection clip has already been automatically created, remove it
    for (int i = collectionClipList->collectionClips.size(); --i >= 0;)
        if (collectionClipList->collectionClips[i]->containsClip (cc->getClip (0).get()))
            collectionClipList->collectionClips.remove (i);

    collectionClipList->collectionClips.add (cc);
}

void ClipTrack::removeCollectionClip (CollectionClip* cc)
{
    collectionClipList->collectionClips.removeObject (cc);
}

//==============================================================================
// The insert* methods are thin wrappers over the free functions in the engine
// namespace (see tracktion_EditUtilities) that do the real work of creating clips,
// finding a free slot, deleting overlaps etc. They're exposed here for convenience
// and to optionally select the newly-created clip via a SelectionManager.
Clip* ClipTrack::insertClipWithState (juce::ValueTree clipState)
{
    return engine::insertClipWithState (*this, clipState);
}

Clip* ClipTrack::insertClipWithState (const juce::ValueTree& stateToUse, const juce::String& name, TrackItem::Type type,
                                      ClipPosition position, bool deleteExistingClips, bool allowSpottingAdjustment)
{
    return engine::insertClipWithState (*this, stateToUse, name, type,
                                        position, deleteExistingClips ? DeleteExistingClips::yes : DeleteExistingClips::no, allowSpottingAdjustment);
}

WaveAudioClip::Ptr ClipTrack::insertWaveClip (const juce::String& name, const juce::File& sourceFile,
                                              ClipPosition position, bool deleteExistingClips)
{
    return engine::insertWaveClip (*this, name, sourceFile, position, deleteExistingClips ? DeleteExistingClips::yes : DeleteExistingClips::no);
}

WaveAudioClip::Ptr ClipTrack::insertWaveClip (const juce::String& name, ProjectItemID sourceID,
                                              ClipPosition position, bool deleteExistingClips)
{
    return engine::insertWaveClip (*this, name, sourceID, position, deleteExistingClips ? DeleteExistingClips::yes : DeleteExistingClips::no);
}

MidiClip::Ptr ClipTrack::insertMIDIClip (const juce::String& name, TimeRange position, SelectionManager* sm)
{
    if (auto newClip = engine::insertMIDIClip (*this, name, position))
    {
        if (sm != nullptr)
        {
            sm->selectOnly (newClip.get());
            sm->keepSelectedObjectsOnScreen();
        }

        return newClip;
    }

    return {};
}

MidiClip::Ptr ClipTrack::insertMIDIClip (TimeRange position, SelectionManager* sm)
{
    return insertMIDIClip (TrackItem::getSuggestedNameForNewItem (TrackItem::Type::midi), position, sm);
}

EditClip::Ptr ClipTrack::insertEditClip (TimeRange position, ProjectItemID sourceID)
{
    return engine::insertEditClip (*this, position, sourceID);
}

// Erases a time range across all clips on the track. Clips wholly inside vanish;
// clips straddling the range are trimmed or split, and any resulting new clips are
// added to the selection.
void ClipTrack::deleteRegion (TimeRange range, SelectionManager* sm)
{
    auto newClips = engine::deleteRegion (*this, range);

    if (sm != nullptr)
        for (auto newClip : newClips)
            sm->addToSelection (newClip);
}

// As deleteRegion, but limited to a single clip (the rest of the track is untouched).
void ClipTrack::deleteRegionOfClip (Clip::Ptr c, TimeRange range, SelectionManager* sm)
{
    jassert (c != nullptr);
    auto newClips = engine::deleteRegion (*c, range);

    if (sm != nullptr)
        for (auto newClip : newClips)
            sm->addToSelection (newClip);
}

Clip* ClipTrack::insertNewClip (TrackItem::Type type, const juce::String& name, TimeRange pos, SelectionManager* sm)
{
    return insertNewClip (type, name, { pos, 0_td }, sm);
}

Clip* ClipTrack::insertNewClip (TrackItem::Type type, const juce::String& name, ClipPosition position, SelectionManager* sm)
{
    CRASH_TRACER

    if (auto newClip = insertClipWithState ({}, name, type, position, false, false))
    {
        if (sm != nullptr)
        {
            sm->selectOnly (newClip);
            sm->keepSelectedObjectsOnScreen();
        }

        return newClip;
    }

    return {};
}

Clip* ClipTrack::insertNewClip (TrackItem::Type type, TimeRange pos, SelectionManager* sm)
{
    return insertNewClip (type, TrackItem::getSuggestedNameForNewItem (type), pos, sm);
}

bool ClipTrack::containsAnyMIDIClips() const
{
    return engine::containsAnyMIDIClips (*this);
}

// ClipOwner interface: tells the shared clip-management code where this owner's
// state lives, who it is, and how to select it.
juce::ValueTree& ClipTrack::getClipOwnerState()
{
    return state;
}

EditItemID ClipTrack::getClipOwnerID()
{
    return itemID;
}

Selectable* ClipTrack::getClipOwnerSelectable()
{
    return this;
}

Edit& ClipTrack::getClipOwnerEdit()
{
    return edit;
}

//==============================================================================
// ClipOwner notification callbacks. The ClipOwner base calls these as its clip
// collection changes so the track can update derived state. Each invalidates the
// cached track-item list; structural changes also break any group freeze (a bounced
// version of the track is no longer valid once its clips change) and broadcast a
// change for listeners/graph rebuild.
void ClipTrack::clipCreated (Clip& c)
{
    if (c.isGrouped())
        collectionClipList->clipCreated (c);

    trackItemsDirty = true;
}

void ClipTrack::clipAddedOrRemoved()
{
    changed();
    setFrozen (false, Track::groupFreeze);
    trackItemsDirty = true;
}

void ClipTrack::clipOrderChanged()
{
    changed();
    setFrozen (false, Track::groupFreeze);
    trackItemsDirty = true;
}

void ClipTrack::clipPositionChanged()
{
    trackItemsDirty = true;
}

// Bumps the trailing number of a string ("Take 1" -> "Take 2"), or appends " 2" if
// there's no trailing number. Used to make unique names for split/duplicated clips.
inline juce::String incrementLastDigit (const juce::String& in)
{
    int digitCount = 0;

    for (int i = in.length(); --i >= 0;)
    {
        if (juce::CharacterFunctions::isDigit (in[i]))
            digitCount++;
        else
            break;
    }

    if (digitCount == 0)
        return in + " 2";

    return in.dropLastCharacters (digitCount)
            + juce::String (in.getTrailingIntValue() + 1);
}

// Splits a single clip in two at 'time', returning the newly-created right-hand clip.
Clip* ClipTrack::splitClip (Clip& clip, const TimePosition time)
{
    return split (clip, time);
}

// Splits every clip on the track that spans 'time'.
void ClipTrack::splitAt (TimePosition time)
{
    engine::split (*this, time);
}

// Shifts every clip whose centre is at/after 'time' later by amountOfSpace,
// inserting a gap (e.g. to make room for new material). Iterating from the end and
// collecting first avoids re-processing clips as the sorted order changes mid-move.
void ClipTrack::insertSpaceIntoTrack (TimePosition time, TimeDuration amountOfSpace)
{
    CRASH_TRACER
    Track::insertSpaceIntoTrack (time, amountOfSpace);

    // make a copied list first, as they'll get moved out-of-order..
    Clip::Array clipsToDo;
    const auto& clips = getClips();

    for (int i = clips.size(); --i >= 0;)
    {
        auto c = clips.getUnchecked (i);

        if (c->getPosition().time.getCentre() >= time)
            clipsToDo.add (c);
        else
            break;
    }

    for (int i = clipsToDo.size(); --i >= 0;)
        if (auto c = clipsToDo.getUnchecked (i).get())
            c->setStart (c->getPosition().getStart() + amountOfSpace, false, true);
}

// Collects all "interesting" times across the track's clips (clip starts/ends,
// loop points etc.) - used to drive snapping and next/previous-edit navigation.
juce::Array<TimePosition> ClipTrack::findAllTimesOfInterest()
{
    juce::Array<TimePosition> cuts;

    for (auto& o : getClips())
        cuts.addArray (o->getInterestingTimes());

    cuts.sort();
    return cuts;
}

// Returns the first time of interest strictly after t (with a small epsilon to
// skip the current position), or the track end if there's nothing further.
TimePosition ClipTrack::getNextTimeOfInterest (TimePosition t)
{
    if (t < TimePosition())
        return TimePosition();

    for (auto c : findAllTimesOfInterest())
        if (c > t + TimeDuration::fromSeconds (0.0001))
            return c;

    return toPosition (getLength());
}

// Returns the last time of interest strictly before t, or an empty position if
// there's nothing earlier.
TimePosition ClipTrack::getPreviousTimeOfInterest (TimePosition t)
{
    if (t < TimePosition())
        return {};

    auto cuts = findAllTimesOfInterest();

    for (int i = cuts.size(); --i >= 0;)
        if (cuts.getUnchecked (i) < t - TimeDuration::fromSeconds (0.0001))
            return cuts.getUnchecked (i);

    return {};
}

// Searches both the track's own plugin chain and any per-clip plugin lists.
bool ClipTrack::containsPlugin (const Plugin* plugin) const
{
    if (pluginList.contains (plugin))
        return true;

    for (auto c : getClips())
        if (auto plugins = c->getPluginList())
            if (plugins->contains (plugin))
                return true;

    return false;
}

// Gathers every plugin reachable from this track: the track chain (via the base
// class), each clip's plugins, and - for container clips - the plugins on their
// nested child clips too.
Plugin::Array ClipTrack::getAllPlugins() const
{
    auto destArray = Track::getAllPlugins();

    for (auto c : getClips())
    {
        destArray.addArray (c->getAllPlugins());

        if (auto containerClip = dynamic_cast<ContainerClip*> (c))
            for (auto childClip : containerClip->getClips())
                destArray.addArray (childClip->getAllPlugins());
    }

    return destArray;
}

void ClipTrack::sendMirrorUpdateToAllPlugins (Plugin& p) const
{
    pluginList.sendMirrorUpdateToAllPlugins (p);

    for (auto c : getClips())
        c->sendMirrorUpdateToAllPlugins (p);
}

// True if any audio clip on the track references the given file - used e.g. to
// decide whether a file is still needed or can be purged from caches.
bool ClipTrack::areAnyClipsUsingFile (const AudioFile& af)
{
    for (auto c : getClips())
        if (auto acb = dynamic_cast<AudioClipBase*> (c))
            if (acb->isUsingFile (af))
                return true;

    return false;
}

}} // namespace tracktion { inline namespace engine
