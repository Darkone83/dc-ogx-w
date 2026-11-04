#ifndef OGXM_REMOTE_JSON_H
#define OGXM_REMOTE_JSON_H

#include <cstddef>
#include <cstdint>

namespace RemoteControl {

// Bounded, allocation-free JSON for the control API. It accepts JSON syntax,
// rejects duplicate object keys, and never reads beyond the supplied body.
class Json {
public:
    enum class Type : uint8_t { Object, Array, String, Number, Boolean, Null };
    struct Token {
        uint16_t begin{};
        uint16_t end{};
        uint16_t next{};
        Type type{};
    };
    static constexpr unsigned MAX_TOKENS = 192;
    static constexpr size_t MAX_BODY = 4096;

    bool parse(const char* data, size_t length);
    int find(int object, const char* key) const;
    bool equals(int token, const char* text) const;
    bool number(int token, double& value) const;
    bool integer(int token, int64_t min, int64_t max, int64_t& value) const;
    bool boolean(int token, bool& value) const;
    const Token& at(unsigned token) const { return tokens_[token]; }
    unsigned count() const { return count_; }
    bool valid(int token) const { return token >= 0 && static_cast<unsigned>(token) < count_; }

private:
    const char* data_{};
    size_t length_{};
    size_t pos_{};
    unsigned count_{};
    Token tokens_[MAX_TOKENS]{};

    void whitespace();
    int value(unsigned depth);
    int string_token();
    bool same_string(int a, int b) const;
};

class JsonWriter {
public:
    JsonWriter(char* output, size_t capacity) : out_(output), capacity_(capacity) {
        if (capacity_) out_[0] = '\0';
    }
    void append(const char* format, ...);
    size_t length() const { return length_; }
    bool ok() const { return ok_; }
private:
    char* out_;
    size_t capacity_;
    size_t length_{};
    bool ok_{true};
};

} // namespace RemoteControl
#endif
