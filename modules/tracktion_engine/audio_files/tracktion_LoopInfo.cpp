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
// LoopInfo stores the musical metadata about an audio file - tempo (as a beat
// count + time signature), root note, one-shot flag, the in/out loop markers and
// any embedded loop/beat points and tags. AudioClipBase uses it to warp/sync a
// sample to the Edit's tempo and pitch.
//
// All state lives in a juce::ValueTree (so it can be persisted in an Edit and
// undone). Because the same underlying tree may be shared between copies, every
// mutator first calls duplicateIfShared() to copy-on-write, and a lock guards
// concurrent access since LoopInfo is read on the audio thread.
//
// The data is populated either by parsing format-specific metadata out of the
// file (ACID/AIFF/REX chunks, Tracktion's own chunk - see init()) or, when no
// metadata exists, by guessing the tempo/key from cue points or the file name
// (deduceTempo()).
//==============================================================================

//==============================================================================
// Constructors — four ways to build a LoopInfo:
//   1. Default: empty state, missing properties initialised to defaults.
//   2. From a juce::File: opens the file, creates a reader, then calls init().
//   3. From an already-open AudioFormatReader: used when the caller already
//      has a reader so the file is not opened twice.
//   4. From a ValueTree: restores persisted loop metadata (e.g. from Edit XML).
LoopInfo::LoopInfo (Engine& e)
    : engine (e), state (IDs::LOOPINFO)
{
    initialiseMissingProps();
}

LoopInfo::LoopInfo (Engine& e, const juce::File& f)
    : engine (e), state (IDs::LOOPINFO)
{
    auto& formatManager = engine.getAudioFileFormatManager();

    if (auto af = formatManager.getFormatFromFileName (f))
    {
        if (auto fin = f.createInputStream())
        {
            const std::unique_ptr<juce::AudioFormatReader> afr (af->createReaderFor (fin.release(), true));
            init (afr.get(), af, f);
        }
    }
}

LoopInfo::LoopInfo (Engine& e, const juce::AudioFormatReader* afr, const juce::AudioFormat* af, const juce::File& f)
    : engine (e), state (IDs::LOOPINFO)
{
    init (afr, af, f);
}

LoopInfo::LoopInfo (Engine& e, const juce::ValueTree& v, juce::UndoManager* u)
    : engine (e), state (v), um (u), maintainParent (v.getParent().isValid())
{
    initialiseMissingProps();
}

LoopInfo::LoopInfo (const LoopInfo& other)
    : engine (other.engine)
{
    *this = other;
}

LoopInfo& LoopInfo::operator= (const LoopInfo& o)
{
    return copyFrom (o.state);
}

//==============================================================================
// Tempo is not stored directly: it's derived from the number of beats over the
// loop's duration. getBpm therefore needs the AudioFileInfo to know the file's
// length/sample rate.
double LoopInfo::getBpm (const AudioFileInfo& wi) const
{
    return getBeatsPerSecond (wi) * 60.0;
}

// Changing the BPM is expressed as a change in beat count: scaling the beats by
// the tempo ratio keeps the same audio length while reporting the new tempo.
void LoopInfo::setBpm (double newBpm, double currentBpm)
{
    if (newBpm != currentBpm)
    {
        jassert (getNumBeats() > 0);
        const double ratio = newBpm / currentBpm;
        setNumBeats (juce::jmax (1.0, getNumBeats() * ratio));
    }
}

void LoopInfo::setBpm (double newBpm, const AudioFileInfo& wi)
{
    // this is a bit of a round about way of calculating the number of beats which
    // will in turn update the BPM but should work until we determine a proper fix

    jassert (wi.sampleRate != 0.0);

    if (wi.sampleRate == 0.0)
        return;

    if (newBpm < 30.0 || newBpm > 1000.0)
        return;

    const double currentBpm = getBpm (wi);

    // If there's no existing tempo there's nothing to scale, so derive the beat
    // count straight from the requested BPM and the file length instead.
    if (currentBpm == 0)
    {
        const auto lengthMins = wi.getLengthInSeconds() / 60.0;
        const auto numBeats = newBpm * lengthMins;
        setNumBeats (numBeats);
    }
    else
    {
        setBpm (newBpm, currentBpm);
    }
}

// Beats-per-second over the looped region (between the in/out markers). Returns a
// fallback of 2.0 (= 120 BPM) whenever the inputs are degenerate so callers never
// divide by zero. The out marker defaults to / is clamped to the file length.
double LoopInfo::getBeatsPerSecond (const AudioFileInfo& wi) const
{
    CRASH_TRACER

    if (wi.sampleRate == 0.0)
        return 2.0;

    auto in  = getInMarker();
    auto out = getOutMarker();

    if (out == -1 || out > wi.lengthInSamples)
        out = wi.lengthInSamples;

    if (out <= 0)
        return 2.0;

    auto length = (out - in) / wi.sampleRate;

    if (length <= 0)
        return 2.0;

    return getNumBeats() / length;
}

//==============================================================================
int LoopInfo::getDenominator() const              { return getProp<int> (IDs::denominator); }
int LoopInfo::getNumerator() const                { return getProp<int> (IDs::numerator); }
void LoopInfo::setDenominator (int dem)           { setProp (IDs::denominator, dem); }
void LoopInfo::setNumerator (int num)             { setProp (IDs::numerator, num); }
double LoopInfo::getNumBeats() const              { return getProp<double> (IDs::numBeats); }
void LoopInfo::setNumBeats (double b)             { setProp (IDs::numBeats, b); }

//==============================================================================
// Loopable means it can be tempo-synced and tiled: it has a valid beat count and
// time signature and isn't flagged as a one-shot (a sound that should play once,
// e.g. a drum hit, rather than being stretched/looped).
bool LoopInfo::isLoopable() const                  { const juce::ScopedLock sl (lock); return ! isOneShot() && getNumBeats() > 0.0 && getDenominator() > 0 && getNumerator() > 0; }
bool LoopInfo::isOneShot() const                   { return getProp<bool> (IDs::oneShot); }

//==============================================================================
// The pitch the sample was recorded at (MIDI note number, -1 if unknown), used to
// transpose it to other pitches.
int LoopInfo::getRootNote() const                  { return getProp<int> (IDs::rootNote); }
void LoopInfo::setRootNote (int note)              { setProp<int> (IDs::rootNote, note); }

//==============================================================================
// Sample positions delimiting the loop within the file. An out marker of -1 means
// "the end of the file".
SampleCount LoopInfo::getInMarker() const          { return getProp<juce::int64> (IDs::inMarker); }
SampleCount LoopInfo::getOutMarker() const         { return getProp<juce::int64> (IDs::outMarker); }

void LoopInfo::setInMarker (SampleCount in)        { setProp<juce::int64> (IDs::inMarker, in); }
void LoopInfo::setOutMarker (SampleCount out)      { setProp<juce::int64> (IDs::outMarker, out); }

//==============================================================================
// Loop points are the per-beat / transient markers embedded in the file (e.g.
// ACID/REX slice points) that the auto-tempo segment builder uses as sync points.
// They are stored as child trees under a LOOPPOINTS node, created on demand.
int LoopInfo::getNumLoopPoints() const
{
    const juce::ScopedLock sl (lock);
    return getLoopPoints().getNumChildren();
}

LoopInfo::LoopPoint LoopInfo::getLoopPoint (int idx) const
{
    const juce::ScopedLock sl (lock);
    auto lp = getLoopPoints().getChild (idx);

    if (lp.isValid())
        return { static_cast<juce::int64> (lp.getProperty (IDs::value)),
                 (LoopPointType) static_cast<int> (lp.getProperty (IDs::type)) };

    return {};
}

void LoopInfo::addLoopPoint (SampleCount pos, LoopPointType type)
{
    const juce::ScopedLock sl (lock);
    duplicateIfShared();

    auto t = createValueTree (IDs::LOOPPOINT,
                              IDs::value, (juce::int64) pos,
                              IDs::type, (int) type);

    getOrCreateLoopPoints().addChild (t, -1, nullptr);
}

void LoopInfo::changeLoopPoint (int idx, SampleCount pos, LoopPointType type)
{
    const juce::ScopedLock sl (lock);
    duplicateIfShared();

    auto lp = getLoopPoints().getChild (idx);
    lp.setProperty (IDs::value, (juce::int64) pos, um);
    lp.setProperty (IDs::type, (int) type, um);
}

void LoopInfo::deleteLoopPoint (int idx)
{
    const juce::ScopedLock sl (lock);
    duplicateIfShared();
    getLoopPoints().removeChild (idx, um);
    removeChildIfEmpty (IDs::LOOPPOINTS);
}

void LoopInfo::clearLoopPoints()
{
    const juce::ScopedLock sl (lock);
    duplicateIfShared();
    state.removeChild (getLoopPoints(), um);
}

// Removes only the loop points of a particular type (e.g. clear auto-detected
// ones while keeping manually-placed markers).
void LoopInfo::clearLoopPoints (LoopPointType type)
{
    const juce::ScopedLock sl (lock);
    auto lps = getLoopPoints();

    for (int i = lps.getNumChildren(); --i >= 0;)
        if (((LoopPointType) int (lps.getChild (i).getProperty (IDs::type))) == type)
            lps.removeChild (i, nullptr);
}

//==============================================================================
// Tags are free-text descriptors parsed from the file metadata (genre, key,
// instrument...) used for browsing/searching loop libraries.
int LoopInfo::getNumTags() const                { const juce::ScopedLock sl (lock); return getTags().getNumChildren(); }
void LoopInfo::clearTags()                      { const juce::ScopedLock sl (lock); duplicateIfShared(); state.removeChild (getTags(), um); }
juce::String LoopInfo::getTag (int idx) const   { const juce::ScopedLock sl (lock); return getTags().getChild (idx).getProperty (IDs::name).toString(); }

void LoopInfo::addTag (const juce::String& tag)
{
    const juce::ScopedLock sl (lock);
    duplicateIfShared();

    juce::ValueTree t (IDs::TAG);
    t.setProperty (IDs::name, tag, um);
    getOrCreateTags().addChild (t, -1, um);
}

void LoopInfo::addTags (const juce::StringArray& tags)
{
    for (const auto& t : tags)
        addTag (t);
}

// Ensures every property the rest of the class reads exists with a sane default,
// so getProp<> never returns an undefined var. Called after construction and after
// parsing file metadata (which may only set a subset).
void LoopInfo::initialiseMissingProps()
{
    const juce::ScopedLock sl (lock);
    setPropertyIfMissing (state, IDs::numBeats, 0.0, um);
    setPropertyIfMissing (state, IDs::denominator, 0, um);
    setPropertyIfMissing (state, IDs::numerator, 0, um);
    setPropertyIfMissing (state, IDs::oneShot, 0, um);
    setPropertyIfMissing (state, IDs::bpm, 0, um);
    setPropertyIfMissing (state, IDs::rootNote, -1, um);
    setPropertyIfMissing (state, IDs::inMarker, 0, um);
    setPropertyIfMissing (state, IDs::outMarker, -1, um);
}

// Deep-copies another tree's contents into our state in place (keeping our own
// tree identity/parent), unlike operator= which replaces it.
LoopInfo& LoopInfo::copyFrom (const juce::ValueTree& o)
{
    const juce::ScopedLock sl (lock);
    copyValueTree (state, o, um);
    return *this;
}

// Tidies up: drops the container node (LOOPPOINTS / TAGS) once its last child has
// been removed, so empty collections don't linger in the serialised state.
void LoopInfo::removeChildIfEmpty (const juce::Identifier& i)
{
    auto v = state.getChildWithName (i);

    if (v.getNumChildren() == 0)
        state.removeChild (v, um);
}

juce::ValueTree LoopInfo::getLoopPoints() const       { const juce::ScopedLock sl (lock); return state.getChildWithName (IDs::LOOPPOINTS); }
juce::ValueTree LoopInfo::getOrCreateLoopPoints()     { const juce::ScopedLock sl (lock); return state.getOrCreateChildWithName (IDs::LOOPPOINTS, um); }

juce::ValueTree LoopInfo::getTags() const             { const juce::ScopedLock sl (lock); return state.getChildWithName (IDs::TAGS); }
juce::ValueTree LoopInfo::getOrCreateTags()           { const juce::ScopedLock sl (lock); return state.getOrCreateChildWithName (IDs::TAGS, um); }

// Copy-on-write. The ValueTree may be shared (e.g. a LoopInfo copied by value), so
// before mutating we make a private copy to avoid changing the other holders'
// state. When maintainParent is set the copy is swapped back into the same slot in
// the parent tree so the LoopInfo stays attached to the document it came from.
void LoopInfo::duplicateIfShared()
{
    const juce::ScopedLock sl (lock);

    if (state.getReferenceCount() <= 1)
        return;

    if (maintainParent)
    {
        auto parent = state.getParent();
        int index = -1;

        auto stateCopy = state.createCopy();

        if (parent.isValid())
        {
            index = parent.indexOf (state);
            parent.removeChild (index, um);
        }

        state = stateCopy;

        if (parent.isValid())
            parent.addChild (state, index, um);
    }
    else
    {
        state = state.createCopy();
    }
}

// Compares formats either by pointer identity or by name, since a file may be read
// through a different AudioFormat instance than the manager's canonical one.
static bool isSameFormat (const juce::AudioFormat* af1, const juce::AudioFormat* af2)
{
    return af1 == af2 || (af1 != nullptr && af2 != nullptr
                           && af1->getFormatName() == af2->getFormatName());
}

// Populates the LoopInfo from a reader's metadata, dispatching on the audio format
// because each one stores tempo/key/loop data in its own chunks:
//   - REX  : tempo + slice (beat) points.
//   - AIFF : Apple loop tags (root note, one-shot, beats, time sig, key tag).
//   - WAV / native : a Tracktion XML chunk if present (used verbatim), otherwise
//                    ACID chunk fields, otherwise generic "tempo"/"time signature"
//                    style string metadata.
//   - fallbacks: bare ACID fields or a sampler MidiUnityNote with no format match.
// If no tempo could be found, it falls back to deduceTempo() from the file name,
// then fills in any still-missing defaults.
void LoopInfo::init (const juce::AudioFormatReader* afr, const juce::AudioFormat* af, const juce::File& file)
{
    if (afr == nullptr || af == nullptr)
        return;

    auto& formatManager = engine.getAudioFileFormatManager();
    const juce::ScopedLock sl (lock);

   #if TRACKTION_ENABLE_REX
    if (isSameFormat (af, formatManager.getRexFormat()))
    {
        setDenominator (afr->metadataValues[RexAudioFormat::rexDenominator].getIntValue());
        setNumerator (afr->metadataValues[RexAudioFormat::rexDenominator].getIntValue());
        const double bpm = afr->metadataValues[RexAudioFormat::rexTempo].getDoubleValue();
        setProp (IDs::bpm, bpm);
        setProp (IDs::numBeats, bpm * ((afr->lengthInSamples / afr->sampleRate) / 60.0));

        juce::StringArray beatPoints;
        beatPoints.addTokens (afr->metadataValues[RexAudioFormat::rexBeatPoints], ";", {});
        beatPoints.removeEmptyStrings();

        for (int i = 0; i < beatPoints.size(); ++i)
            addLoopPoint (beatPoints[i].getIntValue(), LoopInfo::LoopPointType::manual);
    }
    else
   #endif
    if (isSameFormat (af, formatManager.getAiffFormat()))
    {
        const int rootNote = afr->metadataValues[juce::AiffAudioFormat::appleRootSet] == "1"
                                ? afr->metadataValues[juce::AiffAudioFormat::appleRootNote].getIntValue() : -1;

        setRootNote (rootNote);
        setProp (IDs::oneShot, afr->metadataValues[juce::AiffAudioFormat::appleOneShot] == "1");
        setDenominator (afr->metadataValues[juce::AiffAudioFormat::appleDenominator].getIntValue());
        setNumerator (afr->metadataValues[juce::AiffAudioFormat::appleNumerator].getIntValue());
        const double numBeats = afr->metadataValues[juce::AiffAudioFormat::appleBeats].getDoubleValue();
        setProp (IDs::numBeats, numBeats);
        setProp (IDs::bpm, (numBeats * 60.0) / (afr->lengthInSamples / afr->sampleRate));

        juce::StringArray t;
        t.addTokens (afr->metadataValues[juce::AiffAudioFormat::appleTag], ";", {});
        t.add (afr->metadataValues[juce::AiffAudioFormat::appleKey]);
        t.removeEmptyStrings();

        for (int i = 0; i < t.size(); ++i)
            addTag (t[i]);
    }
    else if (isSameFormat (af, formatManager.getWavFormat()))
    {
        auto s = afr->metadataValues[juce::WavAudioFormat::tracktionLoopInfo];

        // Prefer Tracktion's own embedded LoopInfo chunk verbatim; otherwise fall
        // back to interpreting the standard ACID loop chunk.
        if (s.isNotEmpty())
        {
            if (auto n = juce::parseXML (s))
                copyFrom (juce::ValueTree::fromXml (*n));
        }
        else
        {
            const int rootNote = afr->metadataValues[juce::WavAudioFormat::acidRootSet] == "1"
                                    ? afr->metadataValues[juce::WavAudioFormat::acidRootNote].getIntValue() : -1;
            const double numBeats = afr->metadataValues[juce::WavAudioFormat::acidBeats].getDoubleValue();
            
            if (rootNote == -1)
                if (auto smplNote = afr->metadataValues["MidiUnityNote"].getIntValue())
                    setRootNote (smplNote);

            setRootNote (rootNote);
            setNumBeats (numBeats);
            setProp (IDs::oneShot, afr->metadataValues[juce::WavAudioFormat::acidOneShot] == "1");
            setDenominator (afr->metadataValues[juce::WavAudioFormat::acidDenominator].getIntValue());
            setNumerator (afr->metadataValues[juce::WavAudioFormat::acidNumerator].getIntValue());
            setNumBeats (afr->metadataValues[juce::WavAudioFormat::acidBeats].getDoubleValue());
            setProp (IDs::bpm, (numBeats * 60.0) / (afr->lengthInSamples / afr->sampleRate));
        }
    }
    else if (isSameFormat (af, formatManager.getNativeAudioFormat()))
    {
        juce::String s = afr->metadataValues[juce::WavAudioFormat::tracktionLoopInfo];

        if (s.isNotEmpty())
        {
            if (auto n = juce::parseXML (s))
                copyFrom (juce::ValueTree::fromXml (*n));
        }
        else
        {
            const juce::String tempoString = afr->metadataValues["tempo"];
            auto bpm = tempoString.getDoubleValue();

            setProp (IDs::bpm, bpm);

            if (tempoString.isNotEmpty())
            {
                const double fileDuration = afr->lengthInSamples / afr->sampleRate;
                setNumBeats ((fileDuration / 60.0) * bpm);
            }

            const juce::String beatCount = afr->metadataValues["beat count"];

            if (beatCount.isNotEmpty() && bpm == 0)
            {
                const double fileDuration = afr->lengthInSamples / afr->sampleRate;
                const int beats = beatCount.getIntValue();

                setProp (IDs::bpm, beats / fileDuration / 60.0);
                setNumBeats (beats);
            }

            const juce::String timeSig (afr->metadataValues["time signature"]);

            if (timeSig.isNotEmpty())
            {
                setDenominator (timeSig.upToFirstOccurrenceOf ("/", false, false).getIntValue());
                setNumerator (timeSig.fromFirstOccurrenceOf ("/", false, false).getIntValue());
            }

            const juce::String keySig (afr->metadataValues["key signature"]);

            if (keySig.isNotEmpty())
            {
                bool sharpOrFlat = keySig[1] == '#' || keySig[1] == 'b';
                setRootNote (Pitch::getPitchFromString (engine, keySig.substring (0, sharpOrFlat ? 2 : 1)));
                addTag (keySig.getLastCharacter() == 'm' ? "minor" : "major");
            }
        }
    }
    else if (afr->metadataValues.containsKey (juce::WavAudioFormat::acidBeats)
              || afr->metadataValues.containsKey (juce::WavAudioFormat::acidNumerator))
    {
        const int rootNote = afr->metadataValues[juce::WavAudioFormat::acidRootSet] == "1"
                                ? afr->metadataValues[juce::WavAudioFormat::acidRootNote].getIntValue() : -1;
        const double numBeats = afr->metadataValues[juce::WavAudioFormat::acidBeats].getDoubleValue();

        setRootNote (rootNote);
        setNumBeats (numBeats);
        setProp (IDs::oneShot, afr->metadataValues[juce::WavAudioFormat::acidOneShot] == "1");
        setDenominator (afr->metadataValues[juce::WavAudioFormat::acidDenominator].getIntValue());
        setNumerator (afr->metadataValues[juce::WavAudioFormat::acidNumerator].getIntValue());
        setNumBeats (afr->metadataValues[juce::WavAudioFormat::acidBeats].getDoubleValue());
        setProp (IDs::bpm, (numBeats * 60.0) / (afr->lengthInSamples / afr->sampleRate));
    }
    else if (afr->metadataValues.containsKey ("MidiUnityNote"))
    {
        auto note = afr->metadataValues["MidiUnityNote"].getIntValue();
        if (note > 0)
            setRootNote (note);
    }
    
    // Last resort: if no format gave us a tempo, try to guess one from the file.
    if (file != juce::File() && float (state.getProperty (IDs::bpm)) < 0.001f)
        deduceTempo (file, *afr);

    initialiseMissingProps();
}

// Some libraries write the tempo into the first cue-point label as "Tempo: 120".
// Pulls that out if present and sane (50-250 BPM).
std::optional<float> LoopInfo::getCueTempo (const juce::StringPairArray& metadata)
{
    if (auto tempoStr = metadata["CueLabel0Text"]; tempoStr.isNotEmpty())
        if (tempoStr.contains ("Tempo:"))
            if (auto val = tempoStr.fromFirstOccurrenceOf ("Tempo: ", false, false).getFloatValue(); val >= 50 && val <= 250)
                return val;

    return {};
}

// Tokenises then reverses, so callers scan a file name from the end first - the
// tempo/key suffix is usually nearer the end (e.g. "MyLoop_Drums_128bpm_Amin").
static juce::StringArray reverseTokens (juce::StringRef stringToTokenise, juce::StringRef breakCharacters, juce::StringRef quoteCharacters)
{
    auto tokens = juce::StringArray::fromTokens (stringToTokenise, breakCharacters, quoteCharacters);
    std::reverse (tokens.strings.begin(), tokens.strings.end());
    return tokens;
}

// Guesses the tempo from the file name. First looks for a token containing "bpm";
// failing that, accepts any bare integer in the plausible 50-250 range. The exact
// string round-trip check (String (val) == token) rejects things like "0128" or
// values with stray characters.
std::optional<float> LoopInfo::getFileNameTempo (const juce::String& rawName)
{
    auto name = rawName.replace (" ", "_").replace ("-", "_");

    for (auto token : reverseTokens (name, "_", ""))
    {
        if (token.containsIgnoreCase ("bpm"))
        {
            token = token.replace ("bpm", "", true).trim();
            
            while (token.startsWith ("0"))
                token = token.substring (1);
            
            auto val = token.getIntValue();
            
            if (val > 50 && val < 250 && juce::String (val) == token)
                return float (val);
        }
    }
    
    for (auto token : reverseTokens (name, "_", ""))
    {
        auto val = token.getIntValue();
        
        while (token.startsWith ("0"))
            token = token.substring (1);
        
        if (val > 50 && val < 250 && juce::String (val) == token)
            return float (val);
    }

    return {};
}

// Guesses the root note from the file name by matching a trailing key token
// (stripping any "min"/"maj"/"m" suffix), returning the MIDI note in the 4th
// octave. Enharmonics (a#/bb etc.) map to the same note.
std::optional<int> LoopInfo::getFileNameRootNote (const juce::String& rawName)
{
    auto name = rawName.replace (" ", "_").toLowerCase();

    for (auto token : reverseTokens (name, "_", ""))
    {
        if (token.endsWith ("min")) token = token.dropLastCharacters (3);
        else if (token.endsWith ("maj")) token = token.dropLastCharacters (3);
        else if (token.endsWith ("m")) token = token.dropLastCharacters (1);

        if (token == "a")   return 57;
        if (token == "a#")  return 58;
        if (token == "bb")  return 58;
        if (token == "b")   return 59;
        if (token == "c")   return 60;
        if (token == "c#")  return 61;
        if (token == "db")  return 61;
        if (token == "d")   return 62;
        if (token == "d#")  return 63;
        if (token == "eb")  return 63;
        if (token == "e")   return 64;
        if (token == "f")   return 65;
        if (token == "f#")  return 66;
        if (token == "gb")  return 66;
        if (token == "g")   return 67;
        if (token == "g#")  return 68;
        if (token == "ab")  return 68;
    }

    return {};
}

// Best-effort tempo/key detection for files with no proper loop metadata. Tries a
// cue-label tempo, then a tempo embedded in the file name. The result is only
// accepted if it yields a beat count that lands close to a whole bar (a multiple of
// 4), which guards against false positives from arbitrary numbers in the name. On
// success the loop is set up as a 4/4 non-one-shot and the root note is also taken
// from the file name if available.
bool LoopInfo::deduceTempo (const juce::File& file, const juce::AudioFormatReader& afr)
{
    auto len = afr.lengthInSamples / afr.sampleRate;
    if (len <= 1.0 && len > 60.0)
        return false;

    auto fn = file.getFileNameWithoutExtension();

    auto tempo = getCueTempo (afr.metadataValues);
    if (! tempo.has_value())
        tempo = getFileNameTempo (fn);

    if (! tempo.has_value())
        return false;

    // Reject the guess unless the implied beat count is within ~0.1 beat of a bar
    // boundary - a real tempo-matched loop is almost always a whole number of bars.
    auto beats = *tempo / 60 * len;
    auto rem = std::fmod (beats, 4.0f);
    if (rem < 0.0f || (rem > 0.1f && rem < 3.9f) || rem > 4.0f)
        return false;

    setNumBeats (beats);
    setProp (IDs::oneShot, false);
    setDenominator (4);
    setNumerator (4);
    setProp (IDs::bpm, (beats * 60.0) / (afr.lengthInSamples / afr.sampleRate));

    if (auto root = getFileNameRootNote (fn))
    {
        setRootNote (*root);
       #if LOG_DEDUCED_TEMPO
        DBG(fn + juce::String::formatted (": %.1f bpm %.1f beats ", *tempo, beats) + juce::MidiMessage::getMidiNoteName (*root, true, false, 4));
       #endif
    }
    else
    {
       #if LOG_DEDUCED_TEMPO
        DBG(fn + juce::String::formatted (": %.1f bpm %.1f beats", *tempo, beats));
       #endif
    }

    return true;
}

}} // namespace tracktion { inline namespace engine
