#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { name_count = 1024, slot_count = 2048, name_size = 32 };
struct entry { char key[name_size]; int32_t value; };

static uint32_t hash_name(const char *name) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    return hash;
}

static struct entry *locate(struct entry *table, const char *name, uint32_t hash) {
    size_t index = hash & (slot_count - 1);
    while (table[index].key[0] && strcmp(table[index].key, name))
        index = (index + 1) & (slot_count - 1);
    return &table[index];
}

int main(void) {
    char (*names)[name_size] = calloc(name_count, sizeof(*names));
    struct entry *table = calloc(slot_count, sizeof(*table));
#ifdef BENCH_CACHED_HASH
    uint32_t hashes[name_count];
#endif
    if (!names || !table) { free(names); free(table); return 1; }
    for (int32_t i = 0; i < name_count; ++i) {
        int length = snprintf(names[i], name_size, "scene/entity/component-%" PRId32, i);
        if (length < 0 || length >= name_size) return 2;
        const uint32_t hash = hash_name(names[i]);
#ifdef BENCH_CACHED_HASH
        hashes[i] = hash;
#endif
        struct entry *entry = locate(table, names[i], hash);
        if (entry->key[0]) return 3;
        memcpy(entry->key, names[i], (size_t)length + 1);
        entry->value = i * 7 + 3;
    }
    uint64_t sum = 0;
    for (int32_t round = 0; round < 1024; ++round) {
        for (int32_t i = 0; i < name_count; ++i) {
            int32_t k = (i * 31 + round) & 1023;
#ifdef BENCH_CACHED_HASH
            const uint32_t hash = hashes[k];
#else
            const uint32_t hash = hash_name(names[k]);
#endif
            const struct entry *entry = locate(table, names[k], hash);
            if (!entry->key[0]) return 4;
            sum += (uint64_t)entry->value;
        }
    }
    printf("%" PRIu64 "\n", sum);
    free(table);
    free(names);
    return 0;
}
