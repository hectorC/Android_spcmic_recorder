#include "wav_writer.h"

#include <android/log.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#define LOG_TAG "WAVWriter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

WAVWriter::WAVWriter()
    : m_file(nullptr)
    , m_ownedFd(-1)
    , m_sampleRate(0)
    , m_channels(0)
    , m_bitsPerSample(0)
    , m_bytesPerSample(0)
    , m_blockAlign(0)
    , m_byteRate(0)
    , m_dataSize(0)
    , m_totalFrames(0)
    , m_dataSizePos(0)
    , m_ds64ChunkPos(0)
    , m_ds64SizePos(0)
    , m_ds64DataPos(0) {}

WAVWriter::~WAVWriter() {
    close();
}

bool WAVWriter::open(const std::string& filename, int sampleRate, int channels, int bitsPerSample) {
    if (m_file != nullptr) {
        LOGE("WAV file already open");
        return false;
    }

    LOGI("Opening WAV file: %s (%dHz, %dch, %dbit)", filename.c_str(), sampleRate, channels, bitsPerSample);

    m_filename = filename;
    m_ownedFd = -1;
    initializeFormat(sampleRate, channels, bitsPerSample);

    m_file = fopen(filename.c_str(), "w+b");
    if (!m_file) {
        LOGE("Failed to open WAV file: %s (%s)", filename.c_str(), strerror(errno));
        resetState();
        return false;
    }

    if (!writeHeader()) {
        LOGE("Failed to write WAV header");
        fclose(m_file);
        m_file = nullptr;
        resetState();
        return false;
    }

    LOGI("WAV file opened successfully");
    return true;
}

bool WAVWriter::openFromFd(int fd, int sampleRate, int channels, int bitsPerSample) {
    if (m_file != nullptr) {
        LOGE("WAV file already open");
        return false;
    }

    int dupFd = dup(fd);
    if (dupFd < 0) {
        LOGE("Failed to dup file descriptor: %s", strerror(errno));
        resetState();
        return false;
    }

    m_file = fdopen(dupFd, "w+b");
    if (!m_file) {
        LOGE("Failed to fdopen descriptor: %s", strerror(errno));
        ::close(dupFd);
        resetState();
        return false;
    }

    m_ownedFd = dupFd;
    m_filename = "/proc/self/fd/" + std::to_string(dupFd);
    initializeFormat(sampleRate, channels, bitsPerSample);

    if (!writeHeader()) {
        LOGE("Failed to write WAV header via fd");
        fclose(m_file);
        m_file = nullptr;
        resetState();
        return false;
    }

    LOGI("WAV writer opened from fd=%d", dupFd);
    return true;
}

bool WAVWriter::writeData(const uint8_t* data, size_t size) {
    if (!m_file) {
        LOGE("WAV file not open");
        return false;
    }

    if (!data || size == 0) {
        return true;
    }

    size_t written = fwrite(reinterpret_cast<const char*>(data), 1, size, m_file);
    if (written != size) {
        LOGE("Failed to write audio data: wrote %zu of %zu (%s)", written, size, strerror(errno));
        return false;
    }

    m_dataSize += static_cast<uint64_t>(size);
    if (m_blockAlign > 0) {
        m_totalFrames = m_dataSize / static_cast<uint64_t>(m_blockAlign);
    }
    return true;
}

void WAVWriter::close() {
    if (!m_file) {
        return;
    }

    const std::string filenameLog = m_filename;
    LOGI("Closing WAV file: %s (wrote %llu bytes)", filenameLog.c_str(), static_cast<unsigned long long>(m_dataSize));

    if (!updateHeader()) {
        LOGE("Failed to finalize WAV header");
    }

    fflush(m_file);
    fclose(m_file);
    m_file = nullptr;

    if (m_ownedFd >= 0) {
        // fclose already closes the duplicated fd
        m_ownedFd = -1;
    }

    resetState();
    LOGI("WAV file closed successfully");
}

bool WAVWriter::writeHeader() {
    if (!m_file) {
        return false;
    }

    // RIFF header
    if (!writeFourCC("RIFF")) {
        LOGE("Failed to write RIFF tag");
        return false;
    }
    if (!writeUint32(0)) { // Placeholder for RIFF size
        LOGE("Failed to reserve RIFF size");
        return false;
    }
    if (!writeFourCC("WAVE")) {
        LOGE("Failed to write WAVE tag");
        return false;
    }

    // Reserve space for optional ds64 chunk by writing a JUNK chunk immediately after the header.
    m_ds64ChunkPos = ftello(m_file);
    if (m_ds64ChunkPos < 0) {
        LOGE("Failed to get JUNK chunk position: %s", strerror(errno));
        return false;
    }
    if (!writeFourCC("JUNK")) {
        LOGE("Failed to write JUNK placeholder");
        return false;
    }
    m_ds64SizePos = ftello(m_file);
    if (m_ds64SizePos < 0) {
        LOGE("Failed to track JUNK size position: %s", strerror(errno));
        return false;
    }
    if (!writeUint32(DS64_CHUNK_SIZE)) {
        LOGE("Failed to set JUNK size");
        return false;
    }
    m_ds64DataPos = ftello(m_file);
    if (m_ds64DataPos < 0) {
        LOGE("Failed to track JUNK data position: %s", strerror(errno));
        return false;
    }
    if (!writeZeros(DS64_CHUNK_SIZE)) {
        LOGE("Failed to reserve JUNK data");
        return false;
    }

    // fmt chunk (PCM)
    if (!writeFourCC("fmt ")) {
        LOGE("Failed to write fmt tag");
        return false;
    }
    if (!writeUint32(16)) {
        LOGE("Failed to write fmt chunk size");
        return false;
    }
    if (!writeUint16(static_cast<uint16_t>(1))) { // PCM
        LOGE("Failed to write audio format");
        return false;
    }
    if (!writeUint16(static_cast<uint16_t>(m_channels))) {
        LOGE("Failed to write channel count");
        return false;
    }
    if (!writeUint32(static_cast<uint32_t>(m_sampleRate))) {
        LOGE("Failed to write sample rate");
        return false;
    }
    if (!writeUint32(static_cast<uint32_t>(m_byteRate))) {
        LOGE("Failed to write byte rate");
        return false;
    }
    if (!writeUint16(static_cast<uint16_t>(m_blockAlign))) {
        LOGE("Failed to write block align");
        return false;
    }
    if (!writeUint16(static_cast<uint16_t>(m_bitsPerSample))) {
        LOGE("Failed to write bits-per-sample");
        return false;
    }

    // data chunk header
    if (!writeFourCC("data")) {
        LOGE("Failed to write data tag");
        return false;
    }
    m_dataSizePos = ftello(m_file);
    if (m_dataSizePos < 0) {
        LOGE("Failed to get data size position: %s", strerror(errno));
        return false;
    }
    if (!writeUint32(0)) {
        LOGE("Failed to reserve data size");
        return false;
    }

    fflush(m_file);
    return true;
}

bool WAVWriter::updateHeader() {
    if (!m_file) {
        return false;
    }

    off_t currentPos = ftello(m_file);
    if (currentPos == -1) {
        LOGE("Failed to get current position: %s", strerror(errno));
        return false;
    }

    uint64_t fileSize = static_cast<uint64_t>(currentPos);
    uint64_t riffSize64 = (fileSize >= 8) ? (fileSize - 8) : 0;
    uint64_t sampleFrames = (m_blockAlign > 0) ? (m_dataSize / static_cast<uint64_t>(m_blockAlign)) : 0;

    const bool needsRf64 = (m_dataSize > MAX_UINT32) || (riffSize64 > MAX_UINT32);

    if (needsRf64) {
        // Update chunk ID to RF64
        if (fseeko(m_file, 0, SEEK_SET) != 0) {
            LOGE("Failed to seek to RIFF tag: %s", strerror(errno));
            return false;
        }
        if (!writeFourCC("RF64")) {
            LOGE("Failed to write RF64 tag");
            return false;
        }

        // RIFF size placeholder set to max 32-bit
        if (!writeUint32(MAX_UINT32)) {
            LOGE("Failed to set RF64 chunk size");
            return false;
        }

        // Overwrite JUNK placeholder with ds64 chunk
        if (fseeko(m_file, m_ds64ChunkPos, SEEK_SET) != 0) {
            LOGE("Failed to seek to ds64 chunk: %s", strerror(errno));
            return false;
        }
        if (!writeFourCC("ds64")) {
            LOGE("Failed to write ds64 tag");
            return false;
        }
        if (!writeUint32(DS64_CHUNK_SIZE)) {
            LOGE("Failed to write ds64 size");
            return false;
        }
        if (!writeUint64(riffSize64)) {
            LOGE("Failed to write ds64 riff size");
            return false;
        }
        if (!writeUint64(m_dataSize)) {
            LOGE("Failed to write ds64 data size");
            return false;
        }
        if (!writeUint64(sampleFrames)) {
            LOGE("Failed to write ds64 sample count");
            return false;
        }
        if (!writeUint32(0)) { // table length
            LOGE("Failed to write ds64 table length");
            return false;
        }

        // Set data chunk size to max 32-bit
        if (fseeko(m_file, m_dataSizePos, SEEK_SET) != 0) {
            LOGE("Failed to seek to data size for RF64: %s", strerror(errno));
            return false;
        }
        if (!writeUint32(MAX_UINT32)) {
            LOGE("Failed to write RF64 data size placeholder");
            return false;
        }
    } else {
        // Standard RIFF update
        if (fseeko(m_file, 4, SEEK_SET) != 0) {
            LOGE("Failed to seek to RIFF size: %s", strerror(errno));
            return false;
        }
        if (!writeUint32(static_cast<uint32_t>(riffSize64))) {
            LOGE("Failed to write RIFF size");
            return false;
        }

        if (fseeko(m_file, m_dataSizePos, SEEK_SET) != 0) {
            LOGE("Failed to seek to data size: %s", strerror(errno));
            return false;
        }
        if (!writeUint32(static_cast<uint32_t>(m_dataSize))) {
            LOGE("Failed to write data size");
            return false;
        }
    }

    // Restore file position
    if (fseeko(m_file, currentPos, SEEK_SET) != 0) {
        LOGE("Failed to restore position: %s", strerror(errno));
        return false;
    }

    fflush(m_file);
    return true;
}

void WAVWriter::initializeFormat(int sampleRate, int channels, int bitsPerSample) {
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitsPerSample = bitsPerSample;
    m_bytesPerSample = bitsPerSample / 8;
    m_blockAlign = m_channels * m_bytesPerSample;
    m_byteRate = m_sampleRate * m_blockAlign;
    m_dataSize = 0;
    m_totalFrames = 0;
    m_dataSizePos = 0;
    m_ds64ChunkPos = 0;
    m_ds64SizePos = 0;
    m_ds64DataPos = 0;
}

void WAVWriter::resetState() {
    m_filename.clear();
    m_ownedFd = -1;
    m_sampleRate = 0;
    m_channels = 0;
    m_bitsPerSample = 0;
    m_bytesPerSample = 0;
    m_blockAlign = 0;
    m_byteRate = 0;
    m_dataSize = 0;
    m_totalFrames = 0;
    m_dataSizePos = 0;
    m_ds64ChunkPos = 0;
    m_ds64SizePos = 0;
    m_ds64DataPos = 0;
}

bool WAVWriter::writeFourCC(const char* fourcc) {
    return fwrite(fourcc, 1, 4, m_file) == 4;
}

bool WAVWriter::writeUint16(uint16_t value) {
    uint8_t bytes[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF)
    };
    return fwrite(bytes, 1, 2, m_file) == 2;
}

bool WAVWriter::writeUint32(uint32_t value) {
    uint8_t bytes[4] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF)
    };
    return fwrite(bytes, 1, 4, m_file) == 4;
}

bool WAVWriter::writeUint64(uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return fwrite(bytes, 1, 8, m_file) == 8;
}

bool WAVWriter::writeZeros(size_t count) {
    static constexpr size_t BUFFER_SIZE = 64;
    uint8_t zeros[BUFFER_SIZE] = {0};

    size_t remaining = count;
    while (remaining > 0) {
        size_t chunk = remaining < BUFFER_SIZE ? remaining : BUFFER_SIZE;
        if (fwrite(zeros, 1, chunk, m_file) != chunk) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}