#ifndef SPCMIC_MATRIX_IR_DATA_H
#define SPCMIC_MATRIX_IR_DATA_H

#include <vector>

namespace spcmic {

struct MatrixImpulseResponse {
    int sampleRate = 0;
    int irLength = 0;              // Samples per impulse response
    int numInputChannels = 0;      // Typically 84 microphones

    int numOutputChannels = 0;     // Output channel count (e.g. 2 for stereo, 16 for 3OA)
    std::vector<float> impulseData; // Size: numOutputChannels * numInputChannels * irLength

    [[nodiscard]] const float* impulseFor(int outputChannel, int inputChannel) const {
        const size_t offset = (static_cast<size_t>(outputChannel) * static_cast<size_t>(numInputChannels) +
                               static_cast<size_t>(inputChannel)) * static_cast<size_t>(irLength);
        return impulseData.data() + offset;
    }

    [[nodiscard]] bool isValid() const {
        const size_t expectedSize = static_cast<size_t>(numOutputChannels) *
                                    static_cast<size_t>(numInputChannels) *
                                    static_cast<size_t>(irLength);
        return sampleRate > 0 && irLength > 0 &&
               numInputChannels > 0 && numOutputChannels > 0 &&
               impulseData.size() == expectedSize;
    }
};

} // namespace spcmic

#endif // SPCMIC_MATRIX_IR_DATA_H
