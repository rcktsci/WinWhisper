#include "audio_encode.h"

// Instantiate the codec GUIDs (CLSID_AACMFTEncoder) in this translation unit
// instead of linking the media-foundation UUID library.
#include <initguid.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>   // CLSID_AACMFTEncoder

#include "resampler.h"
#include "util.h"
#include "wav.h"

namespace audioenc {

// Defined after the anonymous namespace, used from Encode().
void LogEncoderInputTypes();

namespace {

// Releases a COM pointer on scope exit.
template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr()
    {
        if (p != nullptr)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    void Reset()
    {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }
};

struct MfSession {
    bool started = false;
    MfSession() { started = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)); }
    ~MfSession()
    {
        if (started)
            ::MFShutdown();
    }
};

CStringW TempFilePath()
{
    WCHAR dir[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(dir), dir);
    CStringW path(dir);
    path += L"WinWhisper-upload.m4a";
    return path;
}

bool ReadWholeFile(const CStringW& path, std::vector<uint8_t>* out)
{
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path, L"rb") != 0 || file == nullptr)
        return false;
    ::fseek(file, 0, SEEK_END);
    const long size = ::ftell(file);
    ::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        ::fclose(file);
        return false;
    }
    out->resize(static_cast<size_t>(size));
    const size_t read = ::fread(out->data(), 1, out->size(), file);
    ::fclose(file);
    out->resize(read);
    return read > 0;
}

}  // namespace

// Diagnostic: what input formats does the Microsoft AAC encoder actually offer?
void LogEncoderInputTypes()
{
    ComPtr<IMFTransform> encoder;
    const HRESULT hr = ::CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&encoder));
    if (FAILED(hr) || !encoder) {
        util::Log(L"[encode] AAC encoder MFT unavailable (0x%08X)", static_cast<unsigned long>(hr));
        return;
    }
    for (DWORD i = 0; i < 8; ++i) {
        IMFMediaType* type = nullptr;
        if (FAILED(encoder->GetInputAvailableType(0, i, &type)) || type == nullptr) {
            util::Log(L"[encode] input types listed: %u", i);
            break;
        }
        GUID subtype = {};
        UINT32 rate = 0, channels = 0, bits = 0;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        util::Log(L"[encode]   subtype=%08X-%04X rate=%u ch=%u bits=%u", subtype.Data1,
                  subtype.Data2, rate, channels, bits);
        type->Release();
    }
}

bool M4aAvailable()
{
    MfSession session;
    if (!session.started)
        return false;
    ComPtr<IMFTransform> encoder;
    const HRESULT hr = ::CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&encoder));
    return SUCCEEDED(hr) && encoder.p != nullptr;
}

Format ParseFormat(const CStringW& text)
{
    return text.CompareNoCase(L"wav") == 0 ? Format::Wav : Format::M4a;
}

CStringW FormatName(Format format)
{
    return format == Format::Wav ? L"wav" : L"m4a";
}

Upload BuildUpload(const std::vector<int16_t>& pcm, Format format, int bitrate)
{
    Upload upload;
    CStringW error;
    if (format == Format::M4a && Encode(pcm, 16000, format, bitrate, &upload.bytes, &error)) {
        upload.fileName = L"audio.m4a";
        upload.contentType = L"audio/mp4";
        return upload;
    }

    if (format == Format::M4a && !error.IsEmpty())
        util::Log(L"[encode] falling back to WAV: %s", error.GetString());

    upload.bytes = MakeWav16kMono(pcm);
    upload.fileName = L"audio.wav";
    upload.contentType = L"audio/wav";
    return upload;
}

bool Encode(const std::vector<int16_t>& pcm, int sampleRate, Format format, int bitrate,
            std::vector<uint8_t>* out, CStringW* error)
{
    if (format != Format::M4a || pcm.empty())
        return false;

    MfSession session;
    if (!session.started) {
        if (error != nullptr)
            *error = L"Media Foundation недоступен";
        return false;
    }

    // The Microsoft AAC encoder only advertises 44.1 kHz inputs, so speech has
    // to be resampled up before it will look at it. The encoded size depends on
    // the bitrate, not on the input rate, so the upload stays small.
    constexpr int kEncoderRate = 44100;
    std::vector<int16_t> samples;
    if (sampleRate == kEncoderRate) {
        samples = pcm;
    } else {
        std::vector<float> floats(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i)
            floats[i] = pcm[i] / 32768.0f;
        Resampler resampler;
        resampler.Configure(static_cast<double>(sampleRate), kEncoderRate);
        resampler.Process(floats.data(), floats.size(), samples);
    }
    if (samples.empty()) {
        if (error != nullptr)
            *error = L"Не удалось подготовить аудио для AAC";
        return false;
    }

    const CStringW path = TempFilePath();
    ::DeleteFileW(path);

    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = ::MFCreateSinkWriterFromURL(path, nullptr, nullptr, &writer);
    if (FAILED(hr) || !writer) {
        if (error != nullptr)
            *error = L"Не удалось создать M4A-контейнер";
        return false;
    }

    // --- output: AAC-LC, mono, the same rate as the source -------------------
    ComPtr<IMFMediaType> outType;
    if (FAILED(::MFCreateMediaType(&outType)))
        return false;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(kEncoderRate));
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(bitrate / 8));
    outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);   // raw AAC, no ADTS header
    outType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC, level 2

    DWORD streamIndex = 0;
    hr = writer->AddStream(outType.p, &streamIndex);
    if (FAILED(hr)) {
        if (error != nullptr) {
            CStringW text;
            text.Format(L"Кодировщик AAC недоступен (0x%08X)", static_cast<unsigned long>(hr));
            *error = text;
        }
        return false;
    }

    // --- input: plain 16-bit PCM --------------------------------------------
    ComPtr<IMFMediaType> inType;
    if (FAILED(::MFCreateMediaType(&inType)))
        return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(kEncoderRate));
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                      static_cast<UINT32>(kEncoderRate * 2));
    // The encoder refuses PCM without this flag.
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    inType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);

    hr = writer->SetInputMediaType(streamIndex, inType.p, nullptr);
    if (FAILED(hr)) {
        util::Log(L"[encode] SetInputMediaType failed: 0x%08X (%d Hz, mono, 16 bit)",
                  static_cast<unsigned long>(hr), sampleRate);
        LogEncoderInputTypes();
        if (error != nullptr)
            *error = L"AAC-кодировщик не принял PCM-формат";
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr))
        return false;

    // Feed fixed-size chunks: the encoder expects steady sample counts rather
    // than one giant buffer.
    constexpr size_t kChunkFrames = 1024;
    LONGLONG timestamp = 0;
    bool ok = true;
    for (size_t offset = 0; offset < samples.size(); offset += kChunkFrames) {
        const size_t frames = (samples.size() - offset) < kChunkFrames ? samples.size() - offset
                                                                   : kChunkFrames;
        const DWORD bytes = static_cast<DWORD>(frames * sizeof(int16_t));

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(::MFCreateSample(&sample)) || FAILED(::MFCreateMemoryBuffer(bytes, &buffer))) {
            ok = false;
            break;
        }
        BYTE* target = nullptr;
        if (FAILED(buffer->Lock(&target, nullptr, nullptr))) {
            ok = false;
            break;
        }
        std::memcpy(target, samples.data() + offset, bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        sample->AddBuffer(buffer.p);
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration(static_cast<LONGLONG>(frames));

        if (FAILED(writer->WriteSample(streamIndex, sample.p))) {
            ok = false;
            break;
        }
        timestamp += static_cast<LONGLONG>(frames);
    }

    if (ok && FAILED(writer->Finalize()))
        ok = false;

    writer.Reset();   // close the file before reading it back

    if (!ok) {
        ::DeleteFileW(path);
        if (error != nullptr)
            *error = L"Ошибка кодирования AAC";
        return false;
    }

    const bool read = ReadWholeFile(path, out);
    ::DeleteFileW(path);

    if (!read || out->empty()) {
        if (error != nullptr)
            *error = L"Пустой M4A на выходе";
        return false;
    }

    util::Log(L"[encode] AAC %d kbps: %d KB PCM -> %d KB m4a", bitrate / 1000,
              static_cast<int>(pcm.size() * 2 / 1024), static_cast<int>(out->size() / 1024));
    return true;
}

}  // namespace audioenc
