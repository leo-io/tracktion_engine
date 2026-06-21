#pragma once

#include <JuceHeader.h>
#include <iostream>

class WavPlayer  : private juce::AudioSource
{
public:
    WavPlayer()
    {
        formatManager.registerBasicFormats();

        std::cout << "Initialising audio device..." << std::endl;
        auto result = deviceManager.initialise (0, 2, nullptr, true, {}, nullptr);

        if (result.isNotEmpty())
        {
            std::cout << "Device init error: " << result << std::endl;
            deviceInitialised = false;
        }
        else
        {
            std::cout << "Audio device initialised OK." << std::endl;
            deviceInitialised = true;
        }
    }

    ~WavPlayer() override
    {
        shutdown();
    }

    bool loadAndPlay (const juce::File& file)
    {
        if (! deviceInitialised)
        {
            std::cout << "Cannot play: audio device not initialised." << std::endl;
            return false;
        }

        if (! file.existsAsFile())
        {
            std::cout << "File not found: " << file.getFullPathName() << std::endl;
            return false;
        }

        auto* reader = formatManager.createReaderFor (file);

        if (reader == nullptr)
        {
            std::cout << "Unable to open file: " << file.getFullPathName() << std::endl;
            return false;
        }

        std::cout << "Opened: " << reader->sampleRate << " Hz, "
                  << (int) reader->numChannels << " ch, "
                  << reader->lengthInSamples << " samples" << std::endl;

        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset (new juce::AudioFormatReaderSource (reader, true));
        transportSource.setSource (readerSource.get(), 0, nullptr,
                                   reader->sampleRate, reader->numChannels);

        audioPlayer.setSource (this);
        deviceManager.addAudioCallback (&audioPlayer);
        transportSource.start();

        std::cout << "Playing: " << file.getFileName() << std::endl;
        return true;
    }

    void shutdown()
    {
        audioPlayer.setSource (nullptr);
        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset();
        deviceManager.closeAudioDevice();
    }

    bool isPlaying() const { return transportSource.isPlaying(); }

    void waitForPlaybackToFinish()
    {
        while (transportSource.isPlaying())
            juce::Thread::sleep (50);
    }

private:
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        transportSource.prepareToPlay (samplesPerBlockExpected, sampleRate);
    }

    void releaseResources() override
    {
        transportSource.releaseResources();
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        if (transportSource.getTotalLength() > 0)
        {
            juce::AudioSourceChannelInfo transportInfo (bufferToFill.buffer,
                                                        bufferToFill.startSample,
                                                        bufferToFill.numSamples);
            transportSource.getNextAudioBlock (transportInfo);
        }
        else
        {
            bufferToFill.clearActiveBufferRegion();
        }
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transportSource;
    juce::AudioSourcePlayer audioPlayer;
    bool deviceInitialised = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavPlayer)
};
