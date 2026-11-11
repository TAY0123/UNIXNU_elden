/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Included after the SHENG retained log and its writer-lock helpers. */

unsigned elden_usb_read_log(uint64_t *cursor, void *output, unsigned capacity, uint64_t *lost);
void elden_usb_capture_panic(const char *message);

unsigned
elden_usb_read_log(uint64_t *cursor, void *output, unsigned capacity, uint64_t *lost)
{
    if (!capacity) {
        return 0;
    }
    if (capacity > 512) {
        capacity = 512;
    }
    struct sheng_log_record *record = &sheng_log_buffer;
    uint64_t interrupt_state = sheng_log_mask_async();
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&record->writer, &expected, 1, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        sheng_log_restore_async(interrupt_state);
        return 0;
    }
    uint8_t *bytes = output;
    uint64_t position = *cursor;
    uint64_t total = record->total;
    uint64_t tail_start = total > SHENG_LOG_TAIL ? total - SHENG_LOG_TAIL : 0;
    unsigned count = 0;
    while (position < total && count < capacity) {
        if (position < SHENG_LOG_PREFIX) {
            bytes[count++] = record->prefix[position++];
        } else {
            if (position < tail_start) {
                *lost += tail_start - position;
                position = tail_start;
            }
            bytes[count++] = record->tail[position++ % SHENG_LOG_TAIL];
        }
    }
    *cursor = position;
    __atomic_store_n(&record->writer, 0, __ATOMIC_RELEASE);
    sheng_log_restore_async(interrupt_state);
    return count;
}
