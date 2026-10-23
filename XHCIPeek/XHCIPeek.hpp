#pragma once

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>

class XHCIPeek : public IOService {
    OSDeclareDefaultStructors(XHCIPeek)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    enum SnapshotIndex : uint32_t {
        kSnapshotCommand,
        kSnapshotStatus,
        kSnapshotCommandRingLow,
        kSnapshotCommandRingHigh,
        kSnapshotDeviceContextLow,
        kSnapshotConfiguration,
        kSnapshotPort0,
        kSnapshotPort1,
        kSnapshotPort2,
        kSnapshotPort3,
        kSnapshotInterrupterManagement,
        kSnapshotEventRingSize,
        kSnapshotEventRingBaseLow,
        kSnapshotEventRingDequeueLow,
        kSnapshotEventRingDequeueHigh,
        kSnapshotDoorbell,
        kSnapshotMicroframeIndex,
        kSnapshotValueCount
    };

    static bool matched(void *target, void *ref, IOService *service, IONotifier *notifier);
    static void tick(thread_call_param_t owner, thread_call_param_t context);

    void record(const char *line);
    void poll();
    void pollEventRing();
    void pollCommandRing();
    void scheduleNextTick();
    IOMemoryMap *mapPhysical(uint64_t address, uint32_t length);

    IONotifier *notifier {nullptr};
    thread_call_t timer {nullptr};
    IOPCIDevice *controller {nullptr};
    IOMemoryMap *registerMap {nullptr};
    volatile uint8_t *registers {nullptr};
    uint8_t powerManagementOffset {0};

    IOMemoryMap *erstMap {nullptr};
    IOMemoryMap *eventRingMap {nullptr};
    IOMemoryMap *commandRingMap {nullptr};
    uint64_t erstAddress {0};
    uint64_t eventRingAddress {0};
    uint64_t commandRingAddress {0};

    uint32_t previousRegisters[kSnapshotValueCount] {};
    uint32_t previousEventRing[64] {};
    uint32_t previousCommandRing[48] {};
    char history[16000] {};
    char recentHistory[6000] {};
    uint32_t historyLength {0};
    uint32_t recentHistoryLength {0};
    uint32_t skippedPolls {0};
    uint32_t ticks {0};
    bool firstSnapshot {true};
    bool commandRingSearchDone {false};

    void logAncestors();
    void logRegisterSnapshot(const uint32_t *values, uint16_t pciCommand, uint16_t powerState);
    void clearStaleEventFlags(uint32_t command, uint32_t operationalOffset,
                              uint32_t runtimeOffset);
};
