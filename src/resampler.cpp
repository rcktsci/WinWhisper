#include "resampler.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

double Sinc(double x)
{
    if (std::fabs(x) < 1e-9)
        return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

int16_t Clamp16(float v)
{
    const float scaled = v * 32767.0f;
    if (scaled > 32767.0f)
        return 32767;
    if (scaled < -32768.0f)
        return -32768;
    return static_cast<int16_t>(std::lround(scaled));
}

}  // namespace

Resampler::Resampler()
{
    BuildTable();
}

void Resampler::Configure(double inRate, double outRate)
{
    if (inRate <= 0.0 || outRate <= 0.0 || (inRate == inRate_ && outRate == outRate_))
        return;
    Reset();
    inRate_ = inRate;
    outRate_ = outRate;
    step_ = inRate_ / outRate_;
    BuildTable();
}

void Resampler::Reset()
{
    buf_.clear();
    sourceIndex_ = 0;
    pos_ = kTaps / 2.0;
}

void Resampler::BuildTable()
{
    // Downsampling needs the kernel low-passed to the *output* Nyquist, otherwise
    // everything above it folds back down and turns into Whisper's favourite
    // source of phantom words.
    const double cutoff = std::min(1.0, outRate_ / inRate_) * 0.95;

    table_.assign(static_cast<size_t>(kPhases) * kTaps, 0.0f);
    const int half = kTaps / 2;

    for (int p = 0; p < kPhases; ++p) {
        const double frac = static_cast<double>(p) / kPhases;
        float* taps = &table_[static_cast<size_t>(p) * kTaps];

        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k) {
            const double t = static_cast<double>(k - half) - frac;
            const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * k / (kTaps - 1)) +
                             0.08 * std::cos(4.0 * kPi * k / (kTaps - 1));
            const double h = cutoff * Sinc(cutoff * t) * w;
            taps[k] = static_cast<float>(h);
            sum += h;
        }
        if (std::fabs(sum) > 1e-12) {
            for (int k = 0; k < kTaps; ++k)
                taps[k] = static_cast<float>(taps[k] / sum);
        }
    }
}

void Resampler::Process(const float* input, size_t count, std::vector<int16_t>& out)
{
    if (input != nullptr && count > 0)
        buf_.insert(buf_.end(), input, input + count);

    const int half = kTaps / 2;

    while (true) {
        const long long center = static_cast<long long>(std::floor(pos_));
        const long long first = center - half;
        const long long last = center + half;
        const long long bufFirst = sourceIndex_;
        const long long bufLast = sourceIndex_ + static_cast<long long>(buf_.size()) - 1;

        if (first < bufFirst || last > bufLast)
            break;

        double frac = pos_ - static_cast<double>(center);
        if (frac < 0.0)
            frac = 0.0;
        int phase = static_cast<int>(frac * kPhases);
        phase = std::min(std::max(phase, 0), kPhases - 1);

        const float* taps = &table_[static_cast<size_t>(phase) * kTaps];
        const float* window = &buf_[static_cast<size_t>(first - bufFirst)];

        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k)
            acc += window[k] * taps[k];

        out.push_back(Clamp16(acc));
        pos_ += step_;
    }

    // Release the input prefix that every produced sample already passed.
    const long long consumed = static_cast<long long>(std::floor(pos_)) - half - sourceIndex_ - 1;
    if (consumed > 0) {
        const size_t drop = std::min(static_cast<size_t>(consumed), buf_.size());
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(drop));
        sourceIndex_ += static_cast<long long>(drop);
    }
}