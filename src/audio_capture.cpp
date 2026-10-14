#include "audio_capture.h"

#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <cstring>

#include "util.h"

namespace {

// Not declared as a #define by this SDK revision; the documented HRESULT for
// "the user emptied the buffer before we read it".
#ifndef AUDCLNT_E_BUFFER_EMPTY
#define AUDCLNT_E_BUFFER_EMPTY _HRESULT_TYPEDEF_(0x8007011EL)
#endif

// Some drivers take a moment before the first packet arrives, so give a fresh
// stream two full seconds before writing the endpoint off. A tenth of the
// buffer is ~10 ms, so this is ~200 polls.
constexpr int kProbePolls = 200;

CStringW HResultText(HRESULT hr, const wchar_t* what)
{
    CStringW msg;
    msg.Format(L"%s: ошибка 0x%08X", what, static_cast<unsigned long>(hr));
    return msg;
}

}  // namespace

AudioCapture::~AudioCapture()
{
    Stop();
}

bool AudioCapture::Start(AudioCallback onAudio, FailureCallback onFailure)
{
    if (running_.load())
        return true;

    onAudio_ = std::move(onAudio);
    onFailure_ = std::move(onFailure);
    stopRequested_.store(false);

    thread_ = ::CreateThread(nullptr, 0, &AudioCapture::ThreadProc, this, 0, nullptr);
    if (thread_ == nullptr) {
        if (onFailure_)
            onFailure_(L"Не удалось создать поток захвата звука");
        return false;
    }
    return true;
}

void AudioCapture::Stop()
{
    if (thread_ != nullptr) {
        stopRequested_.store(true);
        ::WaitForSingleObject(thread_, 3000);
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }
    running_.store(false);
}

DWORD WINAPI AudioCapture::ThreadProc(LPVOID self)
{
    return static_cast<AudioCapture*>(self)->Run();
}

// Mix formats in the wild: 16-bit PCM (USB mics), 24-bit PCM (Realtek),
// 32-bit PCM or 32-bit float (WASAPI default). Normalise them all to mono
// float in [-1, 1].
void AudioCapture::ConvertToMono(const BYTE* data, UINT32 frames, float* monoOut)
{
    const int channels = channels_ > 0 ? channels_ : 1;
    const float inv = 1.0f / static_cast<float>(channels);

    const bool isFloat = mixExt_.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         mixExt_.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    const int bits = isFloat ? 32 : mixExt_.Format.wBitsPerSample;

    if (isFloat) {
        const float* samples = reinterpret_cast<const float*>(data);
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c)
                sum += samples[f * channels + c];
            monoOut[f] = sum * inv;
        }
    } else if (bits == 16) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(data);
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c)
                sum += static_cast<float>(samples[f * channels + c]) / 32768.0f;
            monoOut[f] = sum * inv;
        }
    } else if (bits == 24) {
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                const BYTE* p = data + (static_cast<size_t>(f) * channels + c) * 3;
                const int32_t value =
                    static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) |
                                         (static_cast<uint32_t>(p[1]) << 16) |
                                         (static_cast<uint32_t>(p[2]) << 24));
                sum += static_cast<float>(value) / 2147483648.0f;
            }
            monoOut[f] = sum * inv;
        }
    } else if (bits == 32) {
        const int32_t* samples = reinterpret_cast<const int32_t*>(data);
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c)
                sum += static_cast<float>(samples[f * channels + c]) / 2147483648.0f;
            monoOut[f] = sum * inv;
        }
    } else {
        for (UINT32 f = 0; f < frames; ++f)
            monoOut[f] = 0.0f;
    }
}

DWORD AudioCapture::Run()
{
    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(comResult);

    auto report = [&](const CStringW& text) {
        util::Log(L"[audio] %s", text.GetString());
        if (onFailure_)
            onFailure_(text);
    };

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        report(HResultText(hr, L"Не удалось открыть перечисление аудиоустройств"));
        if (comInit)
            ::CoUninitialize();
        return 1;
    }

    // This SDK's IMMDeviceEnumerator only offers EnumAudioEndpoints, so walk the
    // collection rather than the friendlier GetCount/Item pair. Only *active*
    // endpoints: disabled and unplugged ones fail Activate with
    // AUDCLNT_E_DEVICE_INVALIDATED and add nothing but noise to the log.
    IMMDeviceCollection* collection = nullptr;
    UINT count = 0;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection != nullptr)
        collection->GetCount(&count);
    util::Log(L"[audio] %u active capture endpoint(s)", count);

    // Try each endpoint until one actually delivers samples. Virtual microphones
    // (Steam, Remote Desktop, OBS virtual audio) accept Initialize and then sit
    // there producing nothing, so "the stream opened" proves very little.
    //
    // The endpoint the user actually picked in Windows is tried first; only then
    // the rest of the enumeration, or a virtual device that happens to sort
    // earlier would silently win.
    std::vector<IMMDevice*> devices;
    {
        IMMDevice* preferred = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &preferred)) &&
            preferred != nullptr) {
            devices.push_back(preferred);
        }
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* candidate = nullptr;
            if (FAILED(collection->Item(i, &candidate)) || candidate == nullptr)
                continue;
            bool duplicate = false;
            for (IMMDevice* existing : devices) {
                if (existing == candidate) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                candidate->Release();
            else
                devices.push_back(candidate);
        }
    }

    for (IMMDevice* device : devices) {

        CStringW name;
        {
            // Friendly name for the log and for user-facing errors; the endpoint
            // id is an opaque GUID chain and tells nobody anything.
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
                PROPVARIANT value;
                ::PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                    name = value.pwszVal;
                }
                ::PropVariantClear(&value);
                store->Release();
            }
            if (name.IsEmpty()) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
                    name = id;
                    ::CoTaskMemFree(id);
                }
            }
        }

        if (CaptureFrom(device, name)) {
            for (IMMDevice* used : devices)
                used->Release();
            if (collection != nullptr)
                collection->Release();
            enumerator->Release();
            if (comInit)
                ::CoUninitialize();
            return 0;
        }
    }

    for (IMMDevice* used : devices)
        used->Release();

    report(L"Ни одно устройство захвата не отдало звук. Проверьте микрофон в настройках "
           L"Windows и убедитесь, что он не отключён.");
    if (collection != nullptr)
        collection->Release();
    enumerator->Release();
    if (comInit)
        ::CoUninitialize();
    return 1;
}

// Opens one endpoint and pumps it. Returns true when the stream actually
// produced audio (or was stopped on purpose).
bool AudioCapture::CaptureFrom(IMMDevice* device, const CStringW& name)
{
    IAudioClient* client = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client));
    if (FAILED(hr) || client == nullptr) {
        util::Log(L"[audio] %s: Activate failed 0x%08X", name.GetString(), hr);
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr) {
        util::Log(L"[audio] %s: GetMixFormat failed 0x%08X", name.GetString(), hr);
        client->Release();
        return false;
    }

    mixFormat_ = mix;
    mixExt_ = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
    sampleRate_ = static_cast<double>(mix->nSamplesPerSec);
    channels_ = mix->nChannels > 0 ? mix->nChannels : 1;
    deviceName_ = name;

    util::Log(L"[audio] trying '%s': tag=0x%04X, %.0f Hz, %u ch, %u bit", name.GetString(),
              mix->wFormatTag, sampleRate_, mix->nChannels, mix->wBitsPerSample);

    // Shared mode, no stream flags, engine-chosen buffer: exactly what the
    // Microsoft sample does, and the most compatible combination there is.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        util::Log(L"[audio] %s: Initialize failed 0x%08X", name.GetString(), hr);
        ::CoTaskMemFree(mix);
        mixFormat_ = nullptr;
        client->Release();
        return false;
    }

    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr) || capture == nullptr) {
        util::Log(L"[audio] %s: GetService failed 0x%08X", name.GetString(), hr);
        ::CoTaskMemFree(mix);
        mixFormat_ = nullptr;
        client->Release();
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        util::Log(L"[audio] %s: Start failed 0x%08X", name.GetString(), hr);
        capture->Release();
        ::CoTaskMemFree(mix);
        mixFormat_ = nullptr;
        client->Release();
        return false;
    }

    // Best-effort note: a muted endpoint produces pure silence and the user
    // would otherwise blame the recogniser.
    {
        IAudioEndpointVolume* volume = nullptr;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&volume))) &&
            volume != nullptr) {
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMute(&muted)) && muted)
                util::Log(L"[audio] capture endpoint is muted");
            volume->Release();
        }
    }

    running_.store(true);
    util::Log(L"[audio] capture started on '%s': %.0f Hz, %d ch", name.GetString(), sampleRate_,
              channels_);

    BYTE* data = nullptr;
    UINT32 available = 0;
    DWORD flags = 0;
    int emptyPolls = 0;
    bool gotAudio = false;
    bool deviceInvalid = false;
    size_t framesReceived = 0;

    while (!stopRequested_.load()) {
        hr = capture->GetNextPacketSize(&available);
        if (FAILED(hr)) {
            util::Log(L"[audio] GetNextPacketSize failed: 0x%08X", hr);
            deviceInvalid = hr == AUDCLNT_E_DEVICE_INVALIDATED;
            break;
        }

        if (available == 0) {
            if (++emptyPolls > kProbePolls && !gotAudio) {
                util::Log(L"[audio] '%s': no samples after %d polls (~%.1f s)", name.GetString(),
                          emptyPolls, emptyPolls * 0.01);
                deviceInvalid = true;
                break;
            }
            // A real sleep: WaitForSingleObject(NULL, ...) returns immediately,
            // which would turn this poll loop into a busy spin.
            ::Sleep(10);
            continue;
        }

        hr = capture->GetBuffer(&data, &available, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_BUFFER_EMPTY)
                continue;
            util::Log(L"[audio] GetBuffer failed: 0x%08X", hr);
            deviceInvalid = hr == AUDCLNT_E_DEVICE_INVALIDATED;
            break;
        }

        if (data != nullptr && available > 0) {
            mono_.resize(available);
            ConvertToMono(data, available, mono_.data());
            if (onAudio_)
                onAudio_(mono_.data(), available);
            framesReceived += available;
            gotAudio = true;
            emptyPolls = 0;
        }

        capture->ReleaseBuffer(available);

        // GetBuffer's flags are AUDCLNT_BUFFERFLAGS_*: 0x1 is a stream
        // discontinuity (normal at start-up), 0x2 means the buffer holds
        // silence, 0x4 a timestamp problem. None of them says the device died -
        // that arrives as AUDCLNT_E_DEVICE_INVALIDATED above - so they are noted
        // and otherwise ignored.
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
            util::Log(L"[audio] buffer discontinuity on '%s'", name.GetString());
    }

    running_.store(false);
    capture->Release();
    client->Stop();
    client->Release();
    if (mixFormat_ != nullptr) {
        ::CoTaskMemFree(const_cast<WAVEFORMATEX*>(mixFormat_));
        mixFormat_ = nullptr;
    }
    util::Log(L"[audio] capture stopped on '%s' (audio=%d, invalid=%d)", name.GetString(),
              gotAudio ? 1 : 0, deviceInvalid ? 1 : 0);

    if (gotAudio)
        return true;

    if (deviceInvalid && onFailure_) {
        onFailure_(CStringW(L"Микрофон «") + name + L"» не отдаёт звук — пробую следующий…");
    }
    return false;
}