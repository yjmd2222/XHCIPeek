#include "XHCIPeek.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/mach_types.h>
#include <string.h>

extern "C" int snprintf(char *, size_t, const char *, ...);

namespace {
constexpr uint32_t kASMediaASM1042 = 0x10421b21;
constexpr uint32_t kBAR0 = kIOPCIConfigBaseAddress0;
constexpr uint32_t kPollIntervalMilliseconds = 100;
constexpr uint32_t kMaximumPolls = 1200;
constexpr uint32_t kUSBRun = 1;
constexpr uint32_t kUSBEINT = 8;
}

OSDefineMetaClassAndStructors(XHCIPeek, IOService)

extern "C" kern_return_t XHCIPeek_start(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

extern "C" kern_return_t XHCIPeek_stop(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

bool XHCIPeek::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    record("start");
    timer = thread_call_allocate(tick, this);
    if (!timer) {
        record("thread call allocation failed");
        IOService::stop(provider);
        return false;
    }

    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    if (!matching) {
        record("PCI matching dictionary allocation failed");
        thread_call_free(timer);
        timer = nullptr;
        IOService::stop(provider);
        return false;
    }

    notifier = IOService::addMatchingNotification(gIOMatchedNotification, matching,
                                                   matched, this, nullptr, 0);
    matching->release();
    if (!notifier) {
        record("PCI matching notification registration failed");
        thread_call_free(timer);
        timer = nullptr;
        IOService::stop(provider);
        return false;
    }
    return true;
}

void XHCIPeek::stop(IOService *provider) {
    if (notifier) {
        notifier->remove();
        notifier = nullptr;
    }
    if (timer) {
        thread_call_cancel_wait(timer);
        thread_call_free(timer);
        timer = nullptr;
    }
    if (commandRingMap) { commandRingMap->release(); commandRingMap = nullptr; }
    if (eventRingMap) { eventRingMap->release(); eventRingMap = nullptr; }
    if (erstMap) { erstMap->release(); erstMap = nullptr; }
    if (registerMap) { registerMap->release(); registerMap = nullptr; }
    registers = nullptr;
    controller = nullptr;
    IOService::stop(provider);
}

bool XHCIPeek::matched(void *target, void *ref, IOService *service, IONotifier *notifier) {
    XHCIPeek *self = static_cast<XHCIPeek *>(target);
    IOPCIDevice *device = OSDynamicCast(IOPCIDevice, service);
    if (!device || self->controller) return true;
    if (device->configRead32(0) != kASMediaASM1042) return true;

    self->record("found ASM1042");
    self->controller = device;
    if (!device->findPCICapability(kIOPCIPowerManagementCapability,
                                   &self->powerManagementOffset)) {
        self->record("no PCI power-management capability");
        self->controller = nullptr;
        return true;
    }

    self->registerMap = device->mapDeviceMemoryWithRegister(kBAR0);
    if (!self->registerMap) {
        self->record("BAR0 map failed");
        self->controller = nullptr;
        return true;
    }
    self->registers = reinterpret_cast<volatile uint8_t *>(
        self->registerMap->getVirtualAddress());

    char line[96];
    snprintf(line, sizeof(line), "BAR0 mapped len %llu",
             static_cast<unsigned long long>(self->registerMap->getLength()));
    self->record(line);
    self->logAncestors();
    thread_call_enter(self->timer);
    return true;
}

void XHCIPeek::logAncestors() {
    for (IORegistryEntry *entry = controller->getParentEntry(gIOServicePlane);
         entry; entry = entry->getParentEntry(gIOServicePlane)) {
        IOPCIDevice *parent = OSDynamicCast(IOPCIDevice, entry);
        if (!parent) continue;

        uint16_t command = parent->configRead16(kIOPCIConfigCommand);
        uint8_t headerType = parent->configRead8(0x0e) & 0x7f;
        uint16_t bridgeControl = headerType == 1 ? parent->configRead16(0x3e) : 0;
        char line[160];
        snprintf(line, sizeof(line), "ANCESTOR %s hdr=%u CMD=%04x BRIDGECTL=%04x",
                 entry->getName(), headerType, command, bridgeControl);
        record(line);

        // Endpoint DMA depends on upstream bridges allowing bus mastering.
        if (headerType == 1 && !(command & 4)) {
            parent->configWrite16(kIOPCIConfigCommand, command | 4 | 2);
            snprintf(line, sizeof(line), "ANCESTOR %s BME was OFF, set -> CMD=%04x",
                     entry->getName(), parent->configRead16(kIOPCIConfigCommand));
            record(line);
        }
    }
}

IOMemoryMap *XHCIPeek::mapPhysical(uint64_t address, uint32_t length) {
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(
        static_cast<IOPhysicalAddress>(address), length, kIODirectionIn);
    if (!descriptor) return nullptr;
    IOMemoryMap *mapping = descriptor->createMappingInTask(
        kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    descriptor->release();
    return mapping;
}

void XHCIPeek::record(const char *line) {
    uint32_t length = static_cast<uint32_t>(strlen(line));
    if (historyLength + length + 2 < sizeof(history)) {
        memcpy(history + historyLength, line, length);
        historyLength += length;
        history[historyLength++] = '\n';
        history[historyLength] = 0;
        setProperty("log", history);
    }

    if (recentHistoryLength + length + 2 >= sizeof(recentHistory)) {
        recentHistoryLength = 0;
        recentHistory[0] = 0;
    }
    memcpy(recentHistory + recentHistoryLength, line, length);
    recentHistoryLength += length;
    recentHistory[recentHistoryLength++] = '\n';
    recentHistory[recentHistoryLength] = 0;
    setProperty("tail", recentHistory);
}

void XHCIPeek::tick(thread_call_param_t owner, thread_call_param_t context) {
    static_cast<XHCIPeek *>(owner)->poll();
}

void XHCIPeek::scheduleNextTick() {
    if (++ticks >= kMaximumPolls) {
        record("done");
        return;
    }
    uint64_t deadline;
    clock_interval_to_deadline(kPollIntervalMilliseconds, 1000 * 1000, &deadline);
    thread_call_enter_delayed(timer, deadline);
}

void XHCIPeek::clearStaleEventFlags(uint32_t command, uint32_t operationalOffset,
                                    uint32_t runtimeOffset) {
    if (command & kUSBRun) return;

    uint32_t status = *reinterpret_cast<volatile uint32_t *>(
        registers + operationalOffset + 4);
    uint32_t iman = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x20);
    uint32_t erdp = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x38);
    if (!(status & kUSBEINT) && !(iman & 1) && !(erdp & 8)) return;

    // These status bits are write-one-to-clear; leave interrupt enable unchanged.
    *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset + 4) = kUSBEINT;
    *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x20) = iman | 3;
    *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x38) = erdp | 8;

    char line[128];
    snprintf(line, sizeof(line), "CLEARED stale EINT/IP/EHB (sts=%08x iman=%x erdp=%08x)",
             status, iman, erdp);
    record(line);
}

void XHCIPeek::logRegisterSnapshot(const uint32_t *v, uint16_t pciCommand,
                                   uint16_t powerState) {
    bool changed = firstSnapshot;
    for (uint32_t i = 0; i < kSnapshotMicroframeIndex; ++i) {
        if (v[i] != previousRegisters[i]) changed = true;
    }
    if (!changed) return;

    uint64_t nanoseconds;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
    char line[256];
    snprintf(line, sizeof(line),
             "t=%llums CMD=%08x STS=%08x CRCR=%08x:%08x DCBAAP=%08x CFG=%08x "
             "PS=%08x %08x %08x %08x IMAN=%x ERSTSZ=%x ERSTBA=%08x "
             "ERDP=%08x:%08x MF=%x PCI=%04x/%04x",
             static_cast<unsigned long long>(nanoseconds / 1000000ULL),
             v[kSnapshotCommand], v[kSnapshotStatus], v[kSnapshotCommandRingHigh],
             v[kSnapshotCommandRingLow], v[kSnapshotDeviceContextLow],
             v[kSnapshotConfiguration], v[kSnapshotPort0], v[kSnapshotPort1],
             v[kSnapshotPort2], v[kSnapshotPort3], v[kSnapshotInterrupterManagement],
             v[kSnapshotEventRingSize], v[kSnapshotEventRingBaseLow],
             v[kSnapshotEventRingDequeueLow], v[kSnapshotEventRingDequeueHigh],
             v[kSnapshotMicroframeIndex], pciCommand, powerState);
    record(line);
    memcpy(previousRegisters, v, sizeof(previousRegisters));
    firstSnapshot = false;
}

void XHCIPeek::poll() {
    if (!controller || !registers) return;

    uint16_t powerState = controller->configRead16(powerManagementOffset + 4);
    uint16_t pciCommand = controller->configRead16(kIOPCIConfigCommand);
    if ((powerState & 3) != 0 || !(pciCommand & 2)) {
        if (skippedPolls++ % 50 == 0) {
            char line[96];
            snprintf(line, sizeof(line), "skip PMCSR=%04x CMD=%04x (#%u)",
                     powerState, pciCommand, skippedPolls);
            record(line);
        }
        scheduleNextTick();
        return;
    }

    uint32_t capabilityLength = registers[0];
    uint32_t runtimeOffset = *reinterpret_cast<volatile uint32_t *>(registers + 0x18);
    uint32_t operationalOffset = capabilityLength;
    uint32_t v[kSnapshotValueCount];
    v[kSnapshotCommand] = *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset);
    v[kSnapshotStatus] = *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset + 4);
    v[kSnapshotCommandRingLow] = *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset + 0x18);
    v[kSnapshotCommandRingHigh] = *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset + 0x1c);
    v[kSnapshotDeviceContextLow] = *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset + 0x30);
    v[kSnapshotConfiguration] = *reinterpret_cast<volatile uint32_t *>(registers + operationalOffset + 0x38);
    for (uint32_t port = 0; port < 4; ++port)
        v[kSnapshotPort0 + port] = *reinterpret_cast<volatile uint32_t *>(
            registers + operationalOffset + 0x400 + 0x10 * port);
    v[kSnapshotInterrupterManagement] = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x20);
    v[kSnapshotEventRingSize] = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x28);
    v[kSnapshotEventRingBaseLow] = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x30);
    v[kSnapshotEventRingDequeueLow] = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x38);
    v[kSnapshotEventRingDequeueHigh] = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset + 0x3c);
    v[kSnapshotDoorbell] = *reinterpret_cast<volatile uint32_t *>(registers + 0x1800);
    v[kSnapshotMicroframeIndex] = *reinterpret_cast<volatile uint32_t *>(registers + runtimeOffset);

    clearStaleEventFlags(v[kSnapshotCommand], operationalOffset, runtimeOffset);
    logRegisterSnapshot(v, pciCommand, powerState);
    pollEventRing();
    pollCommandRing();
    scheduleNextTick();
}
