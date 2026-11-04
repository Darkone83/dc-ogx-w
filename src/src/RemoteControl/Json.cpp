#include "RemoteControl/Json.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace RemoteControl {

void Json::whitespace() {
    while (pos_ < length_ && (data_[pos_] == ' ' || data_[pos_] == '\t' ||
           data_[pos_] == '\r' || data_[pos_] == '\n')) ++pos_;
}

int Json::string_token() {
    if (pos_ >= length_ || data_[pos_] != '"' || count_ == MAX_TOKENS) return -1;
    const unsigned token = count_++;
    tokens_[token].type = Type::String;
    tokens_[token].begin = static_cast<uint16_t>(++pos_);
    while (pos_ < length_) {
        const unsigned char ch = static_cast<unsigned char>(data_[pos_++]);
        if (ch == '"') {
            tokens_[token].end = static_cast<uint16_t>(pos_ - 1);
            tokens_[token].next = static_cast<uint16_t>(count_);
            return static_cast<int>(token);
        }
        if (ch < 0x20) return -1;
        if (ch != '\\') continue;
        if (pos_ == length_) return -1;
        const char escaped = data_[pos_++];
        if (escaped == 'u') {
            for (unsigned i = 0; i < 4; ++i) {
                if (pos_ == length_) return -1;
                const char hex = data_[pos_++];
                if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') ||
                      (hex >= 'A' && hex <= 'F'))) return -1;
            }
        } else if (!std::strchr("\"\\/bfnrt", escaped)) return -1;
    }
    return -1;
}

bool Json::same_string(int a, int b) const {
    const Token& x = tokens_[a];
    const Token& y = tokens_[b];
    return x.end - x.begin == y.end - y.begin &&
           std::memcmp(data_ + x.begin, data_ + y.begin, x.end - x.begin) == 0;
}

int Json::value(unsigned depth) {
    whitespace();
    if (depth > 6 || pos_ == length_ || count_ == MAX_TOKENS) return -1;
    if (data_[pos_] == '"') return string_token();
    const unsigned token = count_++;
    tokens_[token].begin = static_cast<uint16_t>(pos_);
    const char first = data_[pos_];
    if (first == '{' || first == '[') {
        const bool object = first == '{';
        const char closing = object ? '}' : ']';
        tokens_[token].type = object ? Type::Object : Type::Array;
        ++pos_;
        whitespace();
        if (pos_ < length_ && data_[pos_] != closing) {
            while (true) {
                if (object) {
                    const int key = string_token();
                    if (key < 0) return -1;
                    // Earlier keys are followed by one complete value token.
                    for (unsigned prior = token + 1; prior < static_cast<unsigned>(key);) {
                        if (same_string(static_cast<int>(prior), key)) return -1;
                        prior = tokens_[prior + 1].next;
                    }
                    whitespace();
                    if (pos_ == length_ || data_[pos_++] != ':') return -1;
                }
                if (value(depth + 1) < 0) return -1;
                whitespace();
                if (pos_ == length_) return -1;
                if (data_[pos_] == closing) break;
                if (data_[pos_++] != ',') return -1;
                whitespace();
            }
        }
        if (pos_ == length_ || data_[pos_++] != closing) return -1;
    } else if (first == 't' || first == 'f' || first == 'n') {
        const char* literal = first == 't' ? "true" : first == 'f' ? "false" : "null";
        const size_t n = std::strlen(literal);
        if (length_ - pos_ < n || std::memcmp(data_ + pos_, literal, n)) return -1;
        tokens_[token].type = first == 'n' ? Type::Null : Type::Boolean;
        pos_ += n;
    } else {
        tokens_[token].type = Type::Number;
        if (data_[pos_] == '-') ++pos_;
        if (pos_ == length_) return -1;
        if (data_[pos_] == '0') ++pos_;
        else {
            if (data_[pos_] < '1' || data_[pos_] > '9') return -1;
            while (pos_ < length_ && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
        }
        if (pos_ < length_ && data_[pos_] == '.') {
            const size_t digits = ++pos_;
            while (pos_ < length_ && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return -1;
        }
        if (pos_ < length_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < length_ && (data_[pos_] == '+' || data_[pos_] == '-')) ++pos_;
            const size_t digits = pos_;
            while (pos_ < length_ && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return -1;
        }
    }
    tokens_[token].end = static_cast<uint16_t>(pos_);
    tokens_[token].next = static_cast<uint16_t>(count_);
    return static_cast<int>(token);
}

bool Json::parse(const char* data, size_t length) {
    data_ = data;
    length_ = length;
    pos_ = count_ = 0;
    if (!data || !length || length > MAX_BODY) return false;
    if (value(0) != 0) return false;
    whitespace();
    return pos_ == length_;
}

int Json::find(int object, const char* key) const {
    if (!valid(object) || tokens_[object].type != Type::Object) return -1;
    for (unsigned i = static_cast<unsigned>(object) + 1; i < tokens_[object].next;) {
        if (equals(static_cast<int>(i), key)) return static_cast<int>(i + 1);
        i = tokens_[i + 1].next;
    }
    return -1;
}

bool Json::equals(int token, const char* text) const {
    if (!valid(token) || !text) return false;
    const Token& t = tokens_[token];
    const size_t n = std::strlen(text);
    return n == static_cast<size_t>(t.end - t.begin) && std::memcmp(data_ + t.begin, text, n) == 0;
}

bool Json::number(int token, double& value) const {
    if (!valid(token) || tokens_[token].type != Type::Number) return false;
    const Token& t = tokens_[token];
    const size_t n = t.end - t.begin;
    if (!n || n >= 40) return false;
    char number_text[40];
    std::memcpy(number_text, data_ + t.begin, n);
    number_text[n] = '\0';
    char* end = nullptr;
    value = std::strtod(number_text, &end);
    return end == number_text + n && std::isfinite(value);
}

bool Json::integer(int token, int64_t min, int64_t max, int64_t& value) const {
    double parsed;
    if (!number(token, parsed) || parsed < static_cast<double>(min) ||
        parsed > static_cast<double>(max) || std::floor(parsed) != parsed) return false;
    value = static_cast<int64_t>(parsed);
    return true;
}

bool Json::boolean(int token, bool& value) const {
    if (!valid(token)) return false;
    if (tokens_[token].type == Type::Boolean) {
        value = equals(token, "true");
        return true;
    }
    int64_t n;
    if (!integer(token, 0, 1, n)) return false;
    value = n != 0;
    return true;
}

void JsonWriter::append(const char* format, ...) {
    if (!ok_ || length_ >= capacity_) { ok_ = false; return; }
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(out_ + length_, capacity_ - length_, format, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= capacity_ - length_) {
        ok_ = false;
        return;
    }
    length_ += static_cast<size_t>(n);
}

} // namespace RemoteControl
