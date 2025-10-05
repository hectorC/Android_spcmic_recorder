#ifndef SPCMIC_IR_LOADER_H
#define SPCMIC_IR_LOADER_H

#include <string>
#include <android/asset_manager.h>
#include "matrix_convolver/ir_data.h"

namespace spcmic {

class IRLoader {
public:
    IRLoader();

    void setAssetManager(AAssetManager* manager);

    /**
     * Load the binaural impulse response matching the requested sample rate.
     * Supported sample rates: 48000 Hz, 96000 Hz.
     * @param sampleRateHz Recording sample rate to match.
     * @param outIR Filled with impulse response data on success.
     * @return true on success, false otherwise.
     */
    bool loadBinaural(int sampleRateHz, MatrixImpulseResponse& outIR);

private:
    bool loadFromAsset(const std::string& assetName,
                       int expectedSampleRate,
                       MatrixImpulseResponse& outIR);

    static std::string buildBinauralAssetName(int sampleRateHz);

    AAssetManager* assetManager_;
};

} // namespace spcmic

#endif // SPCMIC_IR_LOADER_H
