#pragma once

template<size_t BUFLEN>
class TextBuffer {
public:
    char buffer[BUFLEN];
    size_t end = 0;

    void begin() {
        clear();
    }

    size_t length() {
        return end;
    }

    bool is_full() {
        return length() == capacity();
    }

    bool is_empty() {
        return end == 0;
    }

    bool append_char(char ch) {
        if (length() + 1 > capacity()) {
            return false;
        } else {
            buffer[end] = ch;
            end += 1;
            buffer[end] = '\0';
            return true;
        }
    }

    bool append_str(const char* s) {
        size_t slen = strlen(s);
        if (length() + slen > capacity()) {
            return false;
        } else {
            memcpy(&buffer[end], s, slen);
            end += slen;
            buffer[end] = '\0';
            return true;
        }
    }

    bool truncate(size_t newsize) {
        if (newsize >= length()) {
            return false;
        } else {
            end = newsize;
            buffer[end] = '\0';
            return true;
        }
    }

    const char* get_str() {
        return buffer;
    }

    void clear() {
        end = 0;
        buffer[0] = '\0';
    }

    size_t capacity() const {
        return BUFLEN - 1;
    }

    void trim_end() {
        while (end > 0 &&
               (buffer[end - 1] == ' ' ||
                buffer[end - 1] == '\t' ||
                buffer[end - 1] == '\r' ||
                buffer[end - 1] == '\n')) {
          end -= 1;
        }
        buffer[end] = '\0';
    }

    bool equals(const char* s) {
        return strcmp(get_str(), s) == 0;
    }

    bool starts_with(const char* s) {
        return strncmp(get_str(), s, strlen(s)) == 0;
    }

    int pop_front() {
        if (length() == 0) {
            return -1;
        } else {
            char ch = buffer[0];
            size_t new_length = length() - 1;
            memmove(&buffer[0], &buffer[1], new_length);
            truncate(new_length);
            return ch;
        }
    }

    int peek() {
        if (length() == 0) {
            return -1;
        } else {
            return buffer[0];
        }
    }
};
