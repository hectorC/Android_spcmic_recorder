#ifndef SPCMIC_MATRIX_IR_DATA_H
#define SPCMIC_MATRIX_IR_DATA_H

#include <vector>

namespace spcmic {

struct MatrixImpulseResponse {
    int sampleRate = 0;
    int irLength = 0;              // Samples per impulse response
    int numInputChannels = 0;      // Typically 84 microphones

    std::vector<float> leftEar;    // Size: numInputChannels * irLength
    std::vector<float> rightEar;   // Size: numInputChannels * irLength

    [[nodiscard]] bool isValid() const {
        const size_t expectedSize = static_cast<size_t>(numInputChannels) * static_cast<size_t>(irLength);
        return sampleRate > 0 && irLength > 0 && numInputChannels > 0 &&
               leftEar.size() == expectedSize && rightEar.size() == expectedSize;
    }
};

} // namespace spcmic

#endif // SPCMIC_MATRIX_IR_DATA_H
