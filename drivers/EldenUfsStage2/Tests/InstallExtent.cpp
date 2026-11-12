/* SPDX-License-Identifier: BSD-2-Clause-Patent */
// Exercises the staged NativeUfs GPT parser and command path with file-backed fixtures.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>

constexpr uint32_t kIOReturnNotWritable = 0xe00002c7;
static uint32_t gEldenLastLba;
#include "RootCommandsBridge.cpp"
#define NATIVE_UFS_ROOT_PARTITION_NAME "macosroot"
#include "RootLayout.h"

constexpr unsigned sectorBytes = 4096;
constexpr uint32_t diskLast = 123871231;
constexpr uint32_t installFirst = 5972392, installLast = 62500000;
constexpr uint32_t recoveryTail = 123870975;
using Block = std::array<uint8_t, sectorBytes>;

static std::array<uint8_t, kDmaBytes> dma;
static std::array<uint32_t, 64> registers;
static std::map<uint32_t, Block> media, cached;
static unsigned commands, writes, flushes;
static uint64_t now;

static void issue()
{
    ++commands;
    auto *upiu = dma.data() + kUcdOffset;
    const uint8_t *cdb = upiu + 16;
    uint8_t *data = dma.data() + kDataOffset;
    uint32_t lba = loadBe32(cdb + 2);
    uint32_t count = (uint32_t(cdb[7]) << 8) | cdb[8];
    assert(upiu[0] == 1 && upiu[2] == 0);
    switch (cdb[0]) {
    case 0x28:
        assert(upiu[1] == 0x40 && loadBe32(upiu + 12) == count * sectorBytes);
        for (unsigned i = 0; i < count; ++i) {
            const Block &block = cached.count(lba + i) ? cached.at(lba + i) : media.at(lba + i);
            memcpy(data + i * sectorBytes, block.data(), sectorBytes);
        }
        break;
    case 0x2a:
        assert(upiu[1] == 0x20 && loadBe32(upiu + 12) == count * sectorBytes);
        assert(lba >= installFirst && lba <= installLast && count && count - 1 <= installLast - lba);
        ++writes;
        for (unsigned i = 0; i < count; ++i) {
            Block &block = (cdb[1] & 8) ? media[lba + i] : cached[lba + i];
            memcpy(block.data(), data + i * sectorBytes, sectorBytes);
        }
        break;
    case 0x35:
        assert(upiu[1] == 0 && loadBe32(upiu + 12) == 0 && lba == 0 && count == 0 && cdb[1] == 0);
        ++flushes;
        for (const auto &entry : cached) media[entry.first] = entry.second;
        cached.clear();
        break;
    default:
        assert(false);
    }
    auto *reply = upiu + kRspOffset;
    memset(reply, 0, 512);
    reply[0] = 0x21;
    reply[3] = upiu[3];
    memset(dma.data() + 8, 0, 4);
    registers[0x58 / 4] = 0;
}

static uint64_t io(unsigned operation, uint64_t address, uint64_t value, void *bytes)
{
    switch (operation) {
    case 0: return registers.at(address / 4);
    case 1:
        if (address == 0x20) registers.at(address / 4) &= ~uint32_t(value);
        else registers.at(address / 4) = uint32_t(value);
        if (address == 0x58) issue();
        break;
    case 2:
    case 3: {
        assert(address >= gHost.phys && address - gHost.phys + value <= dma.size());
        auto *at = dma.data() + (address - gHost.phys);
        if (operation == 2) memcpy(at, bytes, value);
        else memcpy(bytes, at, value);
        break;
    }
    case 4: return now;
    case 5: now += value; break;
    default: assert(false);
    }
    return 0;
}

static void startHost()
{
    registers.fill(0);
    registers[0x30 / 4] = 15;
    ufs_test_reset(io, 0x8cb5e9000);
    gEldenLastLba = diskLast;
}

static void load(const char *path, unsigned sector, uint32_t lba)
{
    std::ifstream file(path, std::ios::binary);
    file.seekg(sector * sectorBytes);
    auto &block = media[lba];
    assert(file.read(reinterpret_cast<char *>(block.data()), block.size()));
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    load(argv[1], 1, 1);
    load(argv[1], 2, 2);
    load(argv[2], 4, diskLast);
    load(argv[2], 0, diskLast - 4);
    load(argv[3], 0, recoveryTail);
    startHost();

    RootLayout root;
    EldenUfsExtent extent;
    const char *why = nullptr;
    assert(findRootLayout(&root, &why, true, diskLast, &extent));
    assert(root.manifestStatus == 0 && root.windows == 2 && commands == 5);
    assert(extent.first == installFirst && extent.last == installLast && extent.valid(diskLast));

    media.at(diskLast - 4).back() ^= 1;
    assert(!findRootLayout(&root, &why, true, diskLast, &extent));
    assert(!strcmp(why, "GPT backup partition array differs from primary"));
    assert(findRootLayout(&root, &why, true) && root.manifestStatus == 0);
    media.at(diskLast - 4).back() ^= 1;
    assert(findRootLayout(&root, &why, true, diskLast, &extent));

    std::array<uint8_t, 2 * sectorBytes> data;
    data.fill(0x58);
    data.back() = 0xa9;
    unsigned before = commands;
    assert(hostEldenWrite4k(extent, installFirst - 1, 2, data.data(), false) == kIOReturnBadArgument);
    assert(hostEldenWrite4k(extent, installLast, 2, data.data(), false) == kIOReturnBadArgument);
    assert(hostEldenWrite4k(extent, installFirst, 0, data.data(), false) == kIOReturnBadArgument);
    assert(hostEldenWrite4k({installFirst, diskLast}, installFirst, 1, data.data(), false) == kIOReturnBadArgument);
    const uint8_t cdb[10] = {0x2a, 8, 0, 0, 0, 0, 0, 0, 1, 0};
    uint32_t status;
    assert(hostScsiDir(1, cdb, 10, 1, sectorBytes, 200000, &status, data.data(), &extent) == kIOReturnNotWritable);
    assert(hostScsiDir(0, cdb, 10, 1, sectorBytes, 200000, &status, data.data()) == kIOReturnNotWritable);
    assert(commands == before);

    assert(hostEldenWrite4k(extent, installLast - 1, 2, data.data(), false) == kIOReturnSuccess);
    assert(writes == 1 && cached.size() == 2 && !media.count(installLast));
    assert(hostSyncCache(&extent) == kIOReturnSuccess && flushes == 1 && cached.empty());
    assert(hostEldenWrite4k(extent, installFirst, 1, data.data(), true) == kIOReturnSuccess);
    assert(media.at(installFirst).front() == 0x58 && cached.empty());
    startHost();
    std::array<uint8_t, 2 * sectorBytes> readback;
    assert(hostRead4k(0, installLast - 1, 2, readback.data()) == kIOReturnSuccess);
    assert(readback == data);
    puts("PASS: dual GPT, write bounds, FUA, cache flush and controller restart readback");
}
