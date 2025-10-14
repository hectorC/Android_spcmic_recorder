#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>

/**
 * Lock-free single-producer, single-consumer ring buffer for audio data.
 * Thread-safe for one writer thread and one reader thread.
 * Uses atomic operations to avoid locks and prevent priority inversion.
 */
class LockFreeRingBuffer {
public:
    /**
     * Constructor
     * @param capacity Size of the ring buffer in bytes (should be power of 2 for best performance)
     */
    explicit LockFreeRingBuffer(size_t capacity)
        : m_capacity(capacity)
        , m_writeIndex(0)
        , m_readIndex(0) {
        
        m_buffer = new uint8_t[capacity];
        memset(m_buffer, 0, capacity);
    }
    
    ~LockFreeRingBuffer() {
        delete[] m_buffer;
    }
    
    // Disable copy and move
    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    
    /**
     * Write data to the ring buffer (producer thread)
     * @param data Pointer to data to write
     * @param size Number of bytes to write
     * @return Number of bytes actually written (may be less than requested if buffer is full)
     */
    size_t write(const uint8_t* data, size_t size) {
        if (!data || size == 0) {
            return 0;
        }
        
        const size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        const size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        
        // Calculate available space for writing
        const size_t available = getAvailableWrite(writeIdx, readIdx);
        const size_t toWrite = std::min(size, available);
        
        if (toWrite == 0) {
            return 0; // Buffer is full
        }
        
        // Write in up to two chunks (handle wrap-around)
        const size_t firstChunk = std::min(toWrite, m_capacity - writeIdx);
        memcpy(m_buffer + writeIdx, data, firstChunk);
        
        if (firstChunk < toWrite) {
            // Wrap around to beginning
            const size_t secondChunk = toWrite - firstChunk;
            memcpy(m_buffer, data + firstChunk, secondChunk);
        }
        
        // Update write index (release semantics ensures data is visible before index update)
        const size_t newWriteIdx = (writeIdx + toWrite) % m_capacity;
        m_writeIndex.store(newWriteIdx, std::memory_order_release);
        
        return toWrite;
    }
    
    /**
     * Read data from the ring buffer (consumer thread)
     * @param data Pointer to destination buffer
     * @param size Maximum number of bytes to read
     * @return Number of bytes actually read (may be less than requested if not enough data available)
     */
    size_t read(uint8_t* data, size_t size) {
        if (!data || size == 0) {
            return 0;
        }
        
        const size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        const size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        
        // Calculate available data for reading
        const size_t available = getAvailableRead(writeIdx, readIdx);
        const size_t toRead = std::min(size, available);
        
        if (toRead == 0) {
            return 0; // Buffer is empty
        }
        
        // Read in up to two chunks (handle wrap-around)
        const size_t firstChunk = std::min(toRead, m_capacity - readIdx);
        memcpy(data, m_buffer + readIdx, firstChunk);
        
        if (firstChunk < toRead) {
            // Wrap around to beginning
            const size_t secondChunk = toRead - firstChunk;
            memcpy(data + firstChunk, m_buffer, secondChunk);
        }
        
        // Update read index (release semantics)
        const size_t newReadIdx = (readIdx + toRead) % m_capacity;
        m_readIndex.store(newReadIdx, std::memory_order_release);
        
        return toRead;
    }
    
    /**
     * Get the number of bytes currently available to read
     */
    size_t getAvailableBytes() const {
        const size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        const size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        return getAvailableRead(writeIdx, readIdx);
    }
    
    /**
     * Get the number of bytes of free space available for writing
     */
    size_t getAvailableSpace() const {
        const size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        const size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        return getAvailableWrite(writeIdx, readIdx);
    }
    
    /**
     * Check if the buffer is empty
     */
    bool isEmpty() const {
        return getAvailableBytes() == 0;
    }
    
    /**
     * Check if the buffer is full
     */
    bool isFull() const {
        return getAvailableSpace() == 0;
    }
    
    /**
     * Get the total capacity of the buffer
     */
    size_t getCapacity() const {
        return m_capacity;
    }
    
    /**
     * Reset the buffer (not thread-safe - should only be called when no reading/writing is happening)
     */
    void reset() {
        m_writeIndex.store(0, std::memory_order_release);
        m_readIndex.store(0, std::memory_order_release);
    }
    
private:
    uint8_t* m_buffer;
    const size_t m_capacity;
    
    // Atomic indices for lock-free operation
    std::atomic<size_t> m_writeIndex;
    std::atomic<size_t> m_readIndex;
    
    /**
     * Calculate available bytes for reading
     */
    size_t getAvailableRead(size_t writeIdx, size_t readIdx) const {
        if (writeIdx >= readIdx) {
            return writeIdx - readIdx;
        } else {
            return m_capacity - readIdx + writeIdx;
        }
    }
    
    /**
     * Calculate available space for writing
     * We reserve 1 byte to distinguish full from empty
     */
    size_t getAvailableWrite(size_t writeIdx, size_t readIdx) const {
        const size_t used = getAvailableRead(writeIdx, readIdx);
        return m_capacity - used - 1; // Reserve 1 byte to distinguish full from empty
    }
};
