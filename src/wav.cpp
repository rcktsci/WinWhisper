#include "wav.h"

#include <cstring>

namespace {

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

void PutTag(std::vector<uint8_t>& v, const char* tag)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(tag[i]));
}

}  // namespace

std::vector<uint8_t> MakeWav(uint32_t sampleRate, const std::vector<int16_t>& pcm)
{
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));

    std::vector<uint8_t> wav;
    wav.reserve(44 + dataBytes);

    PutTag(wav, "RIFF");
    PutU32(wav, 36 + dataBytes);
    PutTag(wav, "WAVE");

    PutTag(wav, "fmt ");
    PutU32(wav, 16);            // PCM chunk size
    PutU16(wav, 1);             // format = PCM
    PutU16(wav, 1);             // channels = mono
    PutU32(wav, sampleRate);
    PutU32(wav, sampleRate * sizeof(int16_t));   // byte rate
    PutU16(wav, sizeof(int16_t));               // block align
    PutU16(wav, 16);                            // bits per sample

    PutTag(wav, "data");
    PutU32(wav, dataBytes);

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), raw, raw + dataBytes);
    return wav;
}

std::vector<uint8_t> MakeWav16kMono(const std::vector<int16_t>& pcm)
{
    return MakeWav(16000, pcm);
}