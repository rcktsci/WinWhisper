#pragma once

#include <afxwin.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
//  Shared-mode WASAPI capture running on its own thread.
//
//  The stream is opened as 32-bit IEEE float at the device's native rate; the
//  resampler then takes it to 16 kHz mono. Converting via the audio engine
//  (AUTOCONVERTPCM) keeps us off every resampler Windows ships.
// ---------------------------------------------------------------------------
class AudioCapture {
public:
    using AudioCallback = std::function<void(const float* mono, size_t frames)>;
    using FailureCallback = std::function<void(const CStringW& message)>;

    ~AudioCapture();

    bool Start(AudioCallback onAudio, FailureCallback onFailure);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    double sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    CStringW deviceName() const { return deviceName_; }

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    DWORD Run();
    bool CaptureFrom(IMMDevice* device, const CStringW& name);
    void ConvertToMono(const BYTE* data, UINT32 frames, float* monoOut);

    HANDLE thread_ = nullptr;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};

    AudioCallback onAudio_;
    FailureCallback onFailure_;

    double sampleRate_ = 0.0;
    int channels_ = 0;
    // GetMixFormat hands back a WAVEFORMATEX that is really a full
    // WAVEFORMATEXTENSIBLE; copying it into a plain WAVEFORMATEX would truncate
    // the sub-format GUID and Initialize would reject it.
    const WAVEFORMATEX* mixFormat_ = nullptr;
    WAVEFORMATEXTENSIBLE mixExt_{};
    CStringW deviceName_;

    std::vector<float> mono_;
};