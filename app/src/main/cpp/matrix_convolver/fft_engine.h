#ifndef SPCMIC_FFT_ENGINE_H
#define SPCMIC_FFT_ENGINE_H

#include <complex>
#include <vector>

namespace spcmic {

class FftEngine {
public:
    static bool isPowerOfTwo(size_t n);

    static void forward(std::vector<std::complex<float>>& data);
    static void inverse(std::vector<std::complex<float>>& data);

private:
    static void transform(std::vector<std::complex<float>>& data, bool inverse);
};

} // namespace spcmic

#endif // SPCMIC_FFT_ENGINE_H
