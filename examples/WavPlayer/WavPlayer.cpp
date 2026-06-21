#include <JuceHeader.h>
#include "WavPlayer.h"

int main (int argc, char* argv[])
{
    std::cout << "WavPlayer starting..." << std::endl;

    juce::ScopedJuceInitialiser_GUI init;

    std::cout << "JUCE initialized." << std::endl;

    if (argc < 2)
    {
        std::cout << "Usage: WavPlayer <path-to-wav-file>" << std::endl;
        return 1;
    }

    juce::File wavFile (argv[1]);
    std::cout << "Looking for: " << wavFile.getFullPathName().toStdString() << std::endl;
    std::cout << "File exists: " << (wavFile.existsAsFile() ? "yes" : "no") << std::endl;

    if (! wavFile.existsAsFile())
    {
        std::cout << "Error: File not found" << std::endl;
        return 1;
    }

    std::cout << "Creating player..." << std::endl;
    WavPlayer player;

    std::cout << "Loading and playing..." << std::endl;

    if (! player.loadAndPlay (wavFile))
    {
        std::cout << "Failed to load and play" << std::endl;
        return 1;
    }

    std::cout << "Playing... (press Ctrl+C to stop)" << std::endl;

    while (player.isPlaying())
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    std::cout << "Playback finished." << std::endl;
    return 0;
}
