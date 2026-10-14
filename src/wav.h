#pragma once

#include <cstdint>
#include <vector>

// Wraps 16-bit mono PCM into a RIFF/WAVE container that Whisper servers accept.
std::vector<uint8_t> MakeWav16kMono(const std::vector<int16_t>& pcm);
std::vector<uint8_t> MakeWav(uint32_t sampleRate, const std::vector<int16_t>& pcm);