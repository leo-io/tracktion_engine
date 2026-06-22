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
    A thin "lane" track used purely to display and edit a parameter's automation curve
    as its own row in the arrangement.

    An AutomationTrack holds no clips, plugins or audio of its own (canContainPlugin is
    always false and it produces no output). It exists so a parameter belonging to a
    plugin on a sibling/parent track can be broken out into a dedicated, full-height
    lane instead of being drawn over the owning track. The actual automation data still
    lives on the AutomatableParameter; this track only references which parameter to
    show. @see Track::getCurrentlyShownAutoParam

    @see Track, AutomatableParameter, AutomationCurve
*/
class AutomationTrack  : public Track
{
public:
    AutomationTrack (Edit&, const juce::ValueTree&);
    ~AutomationTrack() override;

    //==============================================================================
    bool isAutomationTrack() const override            { return true; }
    juce::String getSelectableDescription() override;
    bool canContainPlugin (Plugin*) const override     { return false; }
    juce::String getName() const override;

    //==============================================================================
    using Ptr = juce::ReferenceCountedObjectPtr<AutomationTrack>;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationTrack)
};

}} // namespace tracktion { inline namespace engine
