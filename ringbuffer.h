#pragma once

template<typename T, size_t CAPACITY>
class RingBuffer {
public:
    T buffer[CAPACITY];
    size_t next_read = 0;
    size_t next_write = 0;

    void begin() {
        clear();
    }

    void clear() {
        next_read = 0;
        next_write = 0;
    }

    bool is_empty() {
        return next_read == next_write;
    }

    bool is_full() {
        return ((next_write + 1) % CAPACITY) == next_read;
    }

    size_t available() {
        return ((next_write + CAPACITY) - next_read) % CAPACITY;
    }

    bool push_back(T val) {
        if (is_full()) { return false;}
        buffer[next_write] = val;
        next_write = (next_write + 1) % CAPACITY;
        return true;
    }

    bool pop_front(T* dst) {
        if (!peek_front(dst)) { return false; }
        next_read = (next_read + 1) % CAPACITY;
        return true;
    }

    bool peek_front(T* dst) {
        if (is_empty()) { return false; }
        if (dst != nullptr) {
            *dst = buffer[next_read];
        }
        return true;
    }
};
