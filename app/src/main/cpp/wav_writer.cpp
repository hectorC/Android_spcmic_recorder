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
    , m_dataChunkPos(0) {}

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

    m_dataSize += size;
    return true;
}

void WAVWriter::close() {
    if (!m_file) {
        return;
    }

    const std::string filenameLog = m_filename;
    LOGI("Closing WAV file: %s (wrote %zu bytes)", filenameLog.c_str(), m_dataSize);

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

    WAVHeader header;
    initializeHeader(header);

    size_t written = fwrite(&header, sizeof(header), 1, m_file);
    if (written != 1) {
        LOGE("Failed to write WAV header: %s", strerror(errno));
        return false;
    }
    fflush(m_file);

    m_dataChunkPos = DATA_SIZE_OFFSET;
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

    uint32_t riffSize = static_cast<uint32_t>(sizeof(WAVHeader) - 8 + m_dataSize);
    if (fseeko(m_file, RIFF_SIZE_OFFSET, SEEK_SET) != 0) {
        LOGE("Failed to seek to RIFF size: %s", strerror(errno));
        return false;
    }
    if (fwrite(&riffSize, sizeof(riffSize), 1, m_file) != 1) {
        LOGE("Failed to write RIFF size: %s", strerror(errno));
        return false;
    }

    uint32_t dataSize = static_cast<uint32_t>(m_dataSize);
    if (fseeko(m_file, m_dataChunkPos, SEEK_SET) != 0) {
        LOGE("Failed to seek to data chunk: %s", strerror(errno));
        return false;
    }
    if (fwrite(&dataSize, sizeof(dataSize), 1, m_file) != 1) {
        LOGE("Failed to write data size: %s", strerror(errno));
        return false;
    }

    if (fseeko(m_file, currentPos, SEEK_SET) != 0) {
        LOGE("Failed to restore position: %s", strerror(errno));
        return false;
    }

    fflush(m_file);
    return true;
}

void WAVWriter::initializeHeader(WAVHeader& header) {
    std::memset(&header, 0, sizeof(header));

    std::memcpy(header.riffID, "RIFF", 4);
    header.riffSize = sizeof(WAVHeader) - 8;
    std::memcpy(header.waveID, "WAVE", 4);

    std::memcpy(header.formatID, "fmt ", 4);
    header.formatSize = 16;
    header.audioFormat = 1;
    header.numChannels = m_channels;
    header.sampleRate = m_sampleRate;
    header.byteRate = m_byteRate;
    header.blockAlign = m_blockAlign;
    header.bitsPerSample = m_bitsPerSample;

    std::memcpy(header.dataID, "data", 4);
    header.dataSize = 0;
}

void WAVWriter::initializeFormat(int sampleRate, int channels, int bitsPerSample) {
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitsPerSample = bitsPerSample;
    m_bytesPerSample = bitsPerSample / 8;
    m_blockAlign = m_channels * m_bytesPerSample;
    m_byteRate = m_sampleRate * m_blockAlign;
    m_dataSize = 0;
    m_dataChunkPos = 0;
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
    m_dataChunkPos = 0;
}