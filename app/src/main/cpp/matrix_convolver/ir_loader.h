#ifndef SPCMIC_IR_LOADER_H
#define SPCMIC_IR_LOADER_H

#include <string>
#include <android/asset_manager.h>
#include "matrix_convolver/ir_data.h"

namespace spcmic {

enum class IRPreset {
    Binaural = 0,
    Ortf = 1,
    Xy = 2,
    ThirdOrderAmbisonic = 3
};

class IRLoader {
public:
    IRLoader();

    void setAssetManager(AAssetManager* manager);

    /**
     * Load the impulse response matching the requested preset and sample rate.
     * Supported sample rates: 48000 Hz, 96000 Hz.
     */
    bool loadPreset(IRPreset preset, int sampleRateHz, MatrixImpulseResponse& outIR);

private:
    bool loadFromAsset(const std::string& assetName,
                       int expectedSampleRate,
                       MatrixImpulseResponse& outIR);

    static std::string buildAssetName(IRPreset preset, int sampleRateHz);

    AAssetManager* assetManager_;
};

} // namespace spcmic

#endif // SPCMIC_IR_LOADER_H
