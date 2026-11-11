/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830.h"

#include <assert.h>
#include <stdio.h>

static int check_block(void *context, EldenGmuRegion region, uint32_t offset,
                       const uint8_t *payload, size_t bytes)
{
    static const struct {
        EldenGmuRegion region;
        uint32_t offset;
        size_t bytes;
    } expected[] = {
        {EldenGmuItcm, 0, 0x1ec},
        {EldenGmuIcache, 0, 0x15824},
        {EldenGmuDtcm, 0x3f98, 0x28},
        {EldenGmuDtcm, 0x3fdc, 0x18},
    };
    unsigned *count = context;
    assert(*count < sizeof(expected) / sizeof(expected[0]));
    assert(payload && region == expected[*count].region && offset == expected[*count].offset &&
           bytes == expected[*count].bytes);
    ++*count;
    return 1;
}

static int refuse_block(void *context, EldenGmuRegion region, uint32_t offset,
                        const uint8_t *payload, size_t bytes)
{
    check_block(context, region, offset, payload, bytes);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t image[88896];
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(image, 1, sizeof(image), file) == sizeof(image));
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));

    unsigned count = 0;
    assert(EldenAdreno830LoadGmu(ELDEN_ADRENO830_CHIP_ID, image, sizeof(image),
                               check_block, &count) == EldenGmuSuccess && count == 4);
    count = 0;
    assert(EldenAdreno830LoadGmu(0, image, sizeof(image), check_block, &count) ==
           EldenGmuInvalidImage && count == 0);
    assert(EldenAdreno830LoadGmu(ELDEN_ADRENO830_CHIP_ID, image, sizeof(image) - 1,
                               check_block, &count) == EldenGmuInvalidImage && count == 0);
    assert(EldenAdreno830LoadGmu(ELDEN_ADRENO830_CHIP_ID, image, sizeof(image),
                               refuse_block, &count) == EldenGmuVisitorFailed && count == 1);

    count = 0;
    const size_t last_header = sizeof(image) - 0x18 - 16;
    image[last_header] = 0x98; /* overlap the previous DTCM block */
    assert(EldenAdreno830LoadGmu(ELDEN_ADRENO830_CHIP_ID, image, sizeof(image),
                               check_block, &count) == EldenGmuInvalidImage && count == 0);
    image[last_header + 3] = 0xff; /* destination outside all mapped regions */
    assert(EldenAdreno830LoadGmu(ELDEN_ADRENO830_CHIP_ID, image, sizeof(image),
                               check_block, &count) == EldenGmuInvalidImage && count == 0);
    puts("Adreno 830 GMU image and refusal cases passed");
    return 0;
}
