#include "RemoteControl/Json.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace RemoteControl {
namespace {

// A number is limited to 39 characters. After the decimal range checks below,
// its exact numerator/denominator and the binary rounding workspace fit here.
// Keeping these words local avoids Newlib strtod's shared heap workspace in
// the radio callback, including for subnormal and overflow boundary values.
struct DecimalWords {
    uint32_t word[40]{};
    unsigned used{};

    void digit(unsigned value) {
        uint64_t carry = value;
        for (unsigned i = 0; i < used; ++i) {
            carry += uint64_t(word[i]) * 10u;
            word[i] = uint32_t(carry);
            carry >>= 32;
        }
        if (carry) word[used++] = uint32_t(carry);
    }
    unsigned bits() const {
        if (!used) return 0;
        unsigned count = (used - 1) * 32;
        for (uint32_t top = word[used - 1]; top; top >>= 1) ++count;
        return count;
    }
    void shift(unsigned count) {
        if (!used || !count) return;
        const unsigned whole = count / 32, part = count % 32;
        uint32_t carry = 0;
        if (part) {
            for (unsigned i = 0; i < used; ++i) {
                const uint64_t value = (uint64_t(word[i]) << part) | carry;
                word[i] = uint32_t(value);
                carry = uint32_t(value >> 32);
            }
            if (carry) word[used++] = carry;
        }
        for (unsigned i = used; i; --i) word[i - 1 + whole] = word[i - 1];
        for (unsigned i = 0; i < whole; ++i) word[i] = 0;
        used += whole;
    }
    void halve() {
        uint32_t carry = 0;
        for (unsigned i = used; i; --i) {
            const uint32_t next = word[i - 1] << 31;
            word[i - 1] = (word[i - 1] >> 1) | carry;
            carry = next;
        }
        if (used && !word[used - 1]) --used;
    }
    int compare(const DecimalWords& other) const {
        if (used != other.used) return used < other.used ? -1 : 1;
        for (unsigned i = used; i; --i)
            if (word[i - 1] != other.word[i - 1])
                return word[i - 1] < other.word[i - 1] ? -1 : 1;
        return 0;
    }
    void subtract(const DecimalWords& other) {
        uint64_t borrow = 0;
        for (unsigned i = 0; i < used; ++i) {
            const uint64_t value = uint64_t(word[i]) -
                (i < other.used ? other.word[i] : 0u) - borrow;
            word[i] = uint32_t(value);
            borrow = value >> 63;
        }
        while (used && !word[used - 1]) --used;
    }
};

bool decimal_double(const char* text, size_t length, double& output) {
    static_assert(sizeof(double) == sizeof(uint64_t), "JSON requires binary64 double");
    const char* const end = text + length;
    uint64_t sign = 0;
    if (text != end && *text == '-') { sign = uint64_t(1) << 63; ++text; }
    DecimalWords numerator, denominator, trial;
    unsigned significant = 0, fractional = 0;
    const auto take_digit = [&] {
        const unsigned digit = unsigned(*text++ - '0');
        numerator.digit(digit);
        if (significant || digit) ++significant;
    };
    if (text == end || *text < '0' || *text > '9') return false;
    if (*text == '0') {
        take_digit();
        if (text != end && *text >= '0' && *text <= '9') return false;
    } else {
        do { take_digit(); } while (text != end && *text >= '0' && *text <= '9');
    }
    if (text != end && *text == '.') {
        ++text;
        if (text == end || *text < '0' || *text > '9') return false;
        do { take_digit(); ++fractional; }
        while (text != end && *text >= '0' && *text <= '9');
    }
    int decimal_exponent = 0;
    if (text != end && (*text == 'e' || *text == 'E')) {
        ++text;
        bool negative = false;
        if (text != end && (*text == '+' || *text == '-')) negative = *text++ == '-';
        if (text == end || *text < '0' || *text > '9') return false;
        do {
            if (decimal_exponent < 10000)
                decimal_exponent = decimal_exponent * 10 + (*text - '0');
            ++text;
        } while (text != end && *text >= '0' && *text <= '9');
        if (negative) decimal_exponent = -decimal_exponent;
    }
    if (text != end) return false;
    decimal_exponent -= int(fractional);
    const int decimal_order = int(significant) + decimal_exponent;
    if (numerator.used && decimal_order > 309) return false;
    if (!numerator.used || decimal_order < -323) {
        std::memcpy(&output, &sign, sizeof output);
        return true;
    }
    denominator.digit(1);
    for (int i = 0; i < decimal_exponent; ++i) numerator.digit(0);
    for (int i = 0; i > decimal_exponent; --i) denominator.digit(0);
    int exponent = int(numerator.bits()) - int(denominator.bits());
    if (exponent >= 0) {
        trial = denominator;
        trial.shift(unsigned(exponent));
        if (numerator.compare(trial) < 0) --exponent;
    } else {
        trial = numerator;
        trial.shift(unsigned(-exponent));
        if (trial.compare(denominator) < 0) --exponent;
    }
    if (exponent > 1023) return false;
    const bool subnormal = exponent < -1022;
    const int binary_shift = subnormal ? 1074 : 52 - exponent;
    if (binary_shift >= 0) numerator.shift(unsigned(binary_shift));
    else denominator.shift(unsigned(-binary_shift));
    int quotient_bit = int(numerator.bits()) - int(denominator.bits());
    trial = denominator;
    if (quotient_bit >= 0) trial.shift(unsigned(quotient_bit));
    uint64_t quotient = 0;
    for (; quotient_bit >= 0; --quotient_bit) {
        if (numerator.compare(trial) >= 0) {
            numerator.subtract(trial);
            quotient |= uint64_t(1) << unsigned(quotient_bit);
        }
        trial.halve();
    }
    numerator.shift(1);
    const int round = numerator.compare(denominator);
    if (round > 0 || (!round && (quotient & 1u))) ++quotient;
    uint64_t encoded;
    if (subnormal) encoded = sign | quotient;
    else {
        if (quotient == (uint64_t(1) << 53)) { quotient >>= 1; ++exponent; }
        if (exponent > 1023) return false;
        encoded = sign | (uint64_t(exponent + 1023) << 52) |
            (quotient & ((uint64_t(1) << 52) - 1));
    }
    std::memcpy(&output, &encoded, sizeof output);
    return true;
}
} // namespace

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
    return decimal_double(data_ + t.begin, n, value);
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
