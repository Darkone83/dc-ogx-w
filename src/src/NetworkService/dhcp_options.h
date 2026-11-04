// Bounded DHCP option lookup shared with the host parser tests.
#ifndef OGXM_DHCP_OPTIONS_H
#define OGXM_DHCP_OPTIONS_H
#include <stddef.h>
#include <stdint.h>
static inline const uint8_t *dhcp_option_find(const uint8_t *options, size_t length,
                                             uint8_t requested, size_t *value_length) {
    *value_length = 0;
    for (size_t offset = 0; offset < length;) {
        const uint8_t code = options[offset++];
        if (code == 255) return NULL;
        if (code == 0) continue;
        if (offset >= length) return NULL;
        const size_t count = options[offset++];
        if (count > length - offset) return NULL;
        if (code == requested) {
            *value_length = count;
            return options + offset;
        }
        offset += count;
    }
    return NULL;
}
#endif
