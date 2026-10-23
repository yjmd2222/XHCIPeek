#include "XHCIPeek.hpp"

#include <IOKit/IOLib.h>

extern "C" int snprintf(char *, size_t, const char *, ...);

void XHCIPeek::pollEventRing() {
    uint32_t runtimeOffset = *reinterpret_cast<volatile uint32_t *>(registers + 0x18);
    uint64_t tableAddress =
        (static_cast<uint64_t>(*reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x34)) << 32) |
        *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x30);

    if (tableAddress && tableAddress != erstAddress) {
        if (erstMap) { erstMap->release(); erstMap = nullptr; }
        erstMap = mapPhysical(tableAddress, 16);
        erstAddress = tableAddress;
    }
    if (erstMap) {
        volatile uint32_t *entry = reinterpret_cast<volatile uint32_t *>(
            erstMap->getVirtualAddress());
        uint64_t segmentAddress = (static_cast<uint64_t>(entry[1]) << 32) | entry[0];
        if (segmentAddress && segmentAddress != eventRingAddress) {
            if (eventRingMap) { eventRingMap->release(); eventRingMap = nullptr; }
            eventRingMap = mapPhysical(segmentAddress, 256);
            eventRingAddress = segmentAddress;
            char line[112];
            snprintf(line, sizeof(line), "ERST seg=%llx size=%u",
                     static_cast<unsigned long long>(segmentAddress), entry[2] & 0xffff);
            record(line);
        }
    }
    if (!eventRingMap) return;

    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(
        eventRingMap->getVirtualAddress());
    bool changed = false;
    for (uint32_t i = 0; i < 64; ++i)
        if (ring[i] != previousEventRing[i]) changed = true;
    if (!changed) return;

    for (uint32_t i = 0; i < 64; ++i) previousEventRing[i] = ring[i];
    for (uint32_t trb = 0; trb < 16; ++trb) {
        const uint32_t *words = previousEventRing + trb * 4;
        if (!(words[0] | words[1] | words[2] | words[3])) continue;
        char line[128];
        snprintf(line, sizeof(line), "EVT[%u] %08x %08x %08x %08x",
                 trb, words[0], words[1], words[2], words[3]);
        record(line);
    }
}

void XHCIPeek::pollCommandRing() {
    if (!commandRingSearchDone && erstAddress) {
        commandRingSearchDone = true;
        uint64_t base = erstAddress & ~0xfffULL;
        uint64_t lower = base > 0x40000 ? base - 0x40000 : 0;
        uint64_t upper = lower + 0x80000;
        for (uint64_t address = lower; address < upper && !commandRingMap; address += 0x1000) {
            IOMemoryMap *mapping = mapPhysical(address, 0x1000);
            if (!mapping) continue;
            volatile uint32_t *page = reinterpret_cast<volatile uint32_t *>(
                mapping->getVirtualAddress());
            uint64_t linkTarget = (static_cast<uint64_t>(page[0xff4 / 4]) << 32) |
                                  page[0xff0 / 4];
            if (page[0xffc / 4] == 0x1802 && linkTarget == address) {
                commandRingMap = mapping;
                commandRingAddress = address;
            } else {
                mapping->release();
            }
        }
        char line[96];
        snprintf(line, sizeof(line), "cmdring %s %llx",
                 commandRingMap ? "found" : "NOT found",
                 static_cast<unsigned long long>(commandRingAddress));
        record(line);
    }
    if (!commandRingMap) return;

    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(
        commandRingMap->getVirtualAddress());
    bool changed = false;
    for (uint32_t i = 0; i < 48; ++i)
        if (ring[i] != previousCommandRing[i]) changed = true;
    if (!changed) return;

    for (uint32_t i = 0; i < 48; ++i) previousCommandRing[i] = ring[i];
    for (uint32_t trb = 0; trb < 12; ++trb) {
        const uint32_t *words = previousCommandRing + trb * 4;
        if (!(words[0] | words[1] | words[2] | words[3])) continue;
        char line[128];
        snprintf(line, sizeof(line), "CMD[%u] %08x %08x %08x %08x",
                 trb, words[0], words[1], words[2], words[3]);
        record(line);
    }
}
