#pragma once

#include <afxwin.h>

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
//  Optional audio compression before upload.
//
//  16 kHz mono PCM is 32 KB/s, which is nothing on a fast link but adds seconds
//  on a slow uplink - and the server accepts compressed audio. Windows ships an
//  AAC encoder (Media Foundation), so this costs no third-party dependency.
// ---------------------------------------------------------------------------
namespace audioenc {

enum class Format {
    Wav,      // lossless, universally supported
    M4a,      // AAC-LC in an MP4 container via Media Foundation
};

// Compresses `pcm` (mono 16-bit) when the encoder is available. Returns false
// when the caller should fall back to WAV; `error` explains why, once.
bool Encode(const std::vector<int16_t>& pcm, int sampleRate, Format format, int bitrate,
            std::vector<uint8_t>* out, CStringW* error);

// What actually goes on the wire.
struct Upload {
    std::vector<uint8_t> bytes;
    CStringW fileName = L"audio.wav";
    CStringW contentType = L"audio/wav";
};

// Builds the upload for `pcm`, silently falling back to WAV if compression is
// not possible on this machine.
Upload BuildUpload(const std::vector<int16_t>& pcm, Format format, int bitrate);

// "m4a" / "wav" <-> Format
Format ParseFormat(const CStringW& text);
CStringW FormatName(Format format);

// True when the machine can actually produce M4A (Windows 10/11 always can,
// but a stripped Windows Server install may not).
bool M4aAvailable();

}  // namespace audioenc