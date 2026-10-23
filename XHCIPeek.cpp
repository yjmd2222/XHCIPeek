// Read-only register logger for the ASM1042 (1b21:1042) while macOS's own driver runs it. No Lilu, no hooks.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSString.h>
#include <string.h>
extern "C" int snprintf(char *, size_t, const char *, ...);

class XHCIPeek : public IOService {
    OSDeclareDefaultStructors(XHCIPeek)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
private:
    static bool matched(void *target, void *ref, IOService *svc, IONotifier *n);
    static void tick(thread_call_param_t p0, thread_call_param_t p1);
    thread_call_t tc {nullptr};
    IONotifier *notifier {nullptr};
    IOMemoryMap *map {nullptr};
    IOPCIDevice *pci {nullptr};
    uint8_t pmoff {0};
    uint32_t skipped {0};
    IOMemoryMap *erstMap {nullptr}, *ringMap {nullptr};
    uint64_t erstPhys {0}, ringPhys {0};
    uint32_t ringLast[64] {};
    IOMemoryMap *cmdMap {nullptr};
    uint64_t cmdPhys {0};
    uint32_t cmdLast[48] {};
    bool scanned {false};
    IOMemoryMap *mapPhys(uint64_t pa, uint32_t len);
    volatile uint8_t *base {nullptr};
    uint32_t last[40] {};
    bool first {true};
    char hist[16000] {};
    uint32_t hl {0};
    char tail[6000] {};
    uint32_t tl {0};
    void note(const char *line);
    int ticks {0};
};

OSDefineMetaClassAndStructors(XHCIPeek, IOService)

IOMemoryMap *XHCIPeek::mapPhys(uint64_t pa, uint32_t len) {
    IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)pa, len, kIODirectionIn);
    if (!d) return nullptr;
    IOMemoryMap *m = d->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    d->release();
    return m;
}

void XHCIPeek::note(const char *line) {
    uint32_t n = (uint32_t)strlen(line);
    if (hl + n + 2 < sizeof(hist)) {            // "log" keeps the EARLIEST history and then stops growing
        memcpy(hist + hl, line, n); hl += n; hist[hl++] = '\n'; hist[hl] = 0;
        setProperty("log", hist);
    }
    if (tl + n + 2 >= sizeof(tail)) { tl = 0; tail[0] = 0; }   // "tail" is a rolling window of the latest lines
    memcpy(tail + tl, line, n); tl += n; tail[tl++] = '\n'; tail[tl] = 0;
    setProperty("tail", tail);
}

static inline uint32_t rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

bool XHCIPeek::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    note("start");
    tc = thread_call_allocate(tick, this);
    OSDictionary *d = IOService::serviceMatching("IOPCIDevice");
    notifier = IOService::addMatchingNotification(gIOMatchedNotification, d, matched, this, nullptr, 0);
    d->release();
    return true;
}

void XHCIPeek::stop(IOService *provider) {
    if (notifier) notifier->remove();
    if (tc) { thread_call_cancel(tc); thread_call_free(tc); }
    if (map) map->release();
    IOService::stop(provider);
}

bool XHCIPeek::matched(void *target, void *ref, IOService *svc, IONotifier *n) {
    XHCIPeek *self = (XHCIPeek *)target;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, svc);
    if (!pci || self->map) return true;
    uint32_t id = pci->configRead32(0);
    if (id != 0x10421b21) return true;
    self->note("found ASM1042");
    self->pci = pci;
    if (!pci->findPCICapability(kIOPCIPowerManagementCapability, &self->pmoff)) { self->note("no PM cap"); return true; }
    self->map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!self->map) { self->note("map failed"); return true; }
    self->base = (volatile uint8_t *)self->map->getVirtualAddress();
    { char ln[80]; snprintf(ln, sizeof ln, "BAR0 mapped len %llu", (unsigned long long)self->map->getLength()); self->note(ln); }
    // walk up the PCI ancestors: log COMMAND / bridge control; if a bridge has bus-master off, turn it on (DMA from the endpoint cannot pass otherwise)
    for (IORegistryEntry *p = pci->getParentEntry(gIOServicePlane); p; p = p->getParentEntry(gIOServicePlane)) {
        IOPCIDevice *pp = OSDynamicCast(IOPCIDevice, p);
        if (!pp) continue;
        uint16_t c = pp->configRead16(kIOPCIConfigCommand), hdr = pp->configRead8(0x0e) & 0x7f, bc = hdr == 1 ? pp->configRead16(0x3e) : 0;
        char ln[120]; snprintf(ln, sizeof ln, "ANCESTOR %s hdr=%u CMD=%04x BRIDGECTL=%04x", p->getName(), hdr, c, bc); self->note(ln);
        if (hdr == 1 && !(c & 4)) { pp->configWrite16(kIOPCIConfigCommand, c | 4 | 2); snprintf(ln, sizeof ln, "ANCESTOR %s BME was OFF, set -> CMD=%04x", p->getName(), pp->configRead16(kIOPCIConfigCommand)); self->note(ln); }
    }
    thread_call_enter(self->tc);
    return true;
}

void XHCIPeek::tick(thread_call_param_t p0, thread_call_param_t p1) {
    XHCIPeek *s = (XHCIPeek *)p0;
    volatile uint8_t *b = s->base;
    // never touch MMIO unless the device is in D0 with memory decode on (a powered-down link can hang the CPU)
    uint16_t pmcsr = s->pci->configRead16(s->pmoff + 4), cmd = s->pci->configRead16(kIOPCIConfigCommand);
    if ((pmcsr & 3) != 0 || !(cmd & 2)) {
        if (s->skipped++ % 50 == 0) { char ln[80]; snprintf(ln, sizeof ln, "skip PMCSR=%04x CMD=%04x (#%u)", pmcsr, cmd, s->skipped); s->note(ln); }
        if (++s->ticks < 1200) { uint64_t dl; clock_interval_to_deadline(100, 1000 * 1000, &dl); thread_call_enter_delayed(s->tc, dl); }
        return;
    }
    uint32_t cl = b[0], rts = rd(b, 0x18), op = cl;
    uint32_t v[40]; int n = 0;
    v[n++] = rd(b, op + 0);          // USBCMD
    v[n++] = rd(b, op + 4);          // USBSTS
    v[n++] = rd(b, op + 0x18);       // CRCR lo
    v[n++] = rd(b, op + 0x1c);       // CRCR hi
    v[n++] = rd(b, op + 0x30);       // DCBAAP lo
    v[n++] = rd(b, op + 0x38);       // CONFIG
    for (int p = 0; p < 4; p++) v[n++] = rd(b, op + 0x400 + 0x10 * p);
    v[n++] = rd(b, rts + 0x20);      // IR0 IMAN
    v[n++] = rd(b, rts + 0x28);      // ERSTSZ
    v[n++] = rd(b, rts + 0x30);      // ERSTBA lo
    v[n++] = rd(b, rts + 0x38);      // ERDP lo
    v[n++] = rd(b, rts + 0x3c);      // ERDP hi
    v[n++] = rd(b, 0x1800);          // doorbell 0 (reads 0)
    v[n++] = rd(b, rts);             // MFINDEX
    // ASM1042 leaves EINT / IP / EHB set by the firmware; a halted controller must not carry them into Apple's start (event ring "full").
    if (!(v[0] & 1) && v[10] /*unused guard*/ >= 0) {
        uint32_t erdpLo = rd(b, rts + 0x38), iman = rd(b, rts + 0x20), sts = rd(b, op + 4);
        if ((erdpLo & 8) || (iman & 1) || (sts & 8)) {
            wr(b, op + 4, 8);                     // USBSTS.EINT is write-1-to-clear
            wr(b, rts + 0x20, iman | 3);          // IMAN.IP is write-1-to-clear, keep IE set
            wr(b, rts + 0x38, erdpLo | 8);        // ERDP.EHB is write-1-to-clear (pointer bits unchanged)
            char ln[100]; snprintf(ln, sizeof ln, "CLEARED stale EINT/IP/EHB (sts=%08x iman=%x erdp=%08x)", sts, iman, erdpLo); s->note(ln);
        }
    }
    bool chg = s->first;
    for (int i = 0; i < n - 1; i++) if (v[i] != s->last[i]) chg = true;   // skip MFINDEX in change test
    if (chg) {
        uint64_t t; absolutetime_to_nanoseconds(mach_absolute_time(), &t);
        char ln[200];
        snprintf(ln, sizeof ln, "t=%llums CMD=%08x STS=%08x CRCR=%08x:%08x DCBAAP=%08x CFG=%08x PS=%08x %08x %08x %08x IMAN=%x ERSTSZ=%x ERSTBA=%08x ERDP=%08x:%08x MF=%x PCI=%04x/%04x",
              t / 1000000ULL, v[0], v[1], v[3], v[2], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[16], cmd, pmcsr);
        s->note(ln);
        for (int i = 0; i < n; i++) s->last[i] = v[i];
        s->first = false;
    }
    {   // event ring memory: ERST entry -> segment -> first 16 TRBs (only when ERSTBA/ERDP known)
        uint64_t erstba = ((uint64_t)rd(b, rts + 0x34) << 32) | rd(b, rts + 0x30);
        if (erstba && erstba != s->erstPhys) {
            if (s->erstMap) { s->erstMap->release(); s->erstMap = nullptr; }
            s->erstMap = s->mapPhys(erstba, 16); s->erstPhys = erstba;
        }
        if (s->erstMap) {
            volatile uint32_t *e = (volatile uint32_t *)s->erstMap->getVirtualAddress();
            uint64_t seg = ((uint64_t)e[1] << 32) | e[0];
            if (seg && seg != s->ringPhys) {
                if (s->ringMap) { s->ringMap->release(); s->ringMap = nullptr; }
                s->ringMap = s->mapPhys(seg, 256); s->ringPhys = seg;
                char ln[100]; snprintf(ln, sizeof ln, "ERST seg=%llx size=%u", (unsigned long long)seg, e[2] & 0xffff); s->note(ln);
            }
        }
        if (s->ringMap) {
            volatile uint32_t *r = (volatile uint32_t *)s->ringMap->getVirtualAddress();
            bool d = false;
            for (int i = 0; i < 64; i++) if (r[i] != s->ringLast[i]) d = true;
            if (d) {
                for (int i = 0; i < 64; i++) s->ringLast[i] = r[i];
                for (int t = 0; t < 16; t++) {
                    if (!r[t*4] && !r[t*4+1] && !r[t*4+2] && !r[t*4+3]) continue;
                    char ln[120]; snprintf(ln, sizeof ln, "EVT[%d] %08x %08x %08x %08x", t, r[t*4], r[t*4+1], r[t*4+2], r[t*4+3]); s->note(ln);
                }
            }
        }
    }
    {   // find Apple's command ring: a 4 KB page whose Link TRB at +0xff0 points at the page itself (control 0x1802 at +0xffc)
        if (!s->scanned && s->erstPhys) {
            s->scanned = true;
            uint64_t lo = (s->erstPhys & ~0xfffULL) > 0x40000 ? (s->erstPhys & ~0xfffULL) - 0x40000 : 0;
            for (uint64_t pa = lo; pa < lo + 0x80000 && !s->cmdMap; pa += 0x1000) {
                IOMemoryMap *m = s->mapPhys(pa, 0x1000);
                if (!m) continue;
                volatile uint32_t *p = (volatile uint32_t *)m->getVirtualAddress();
                uint64_t ptr = ((uint64_t)p[0xff4 / 4] << 32) | p[0xff0 / 4];
                if (p[0xffc / 4] == 0x1802 && ptr == pa) { s->cmdMap = m; s->cmdPhys = pa; }
                else m->release();
            }
            char ln[80]; snprintf(ln, sizeof ln, "cmdring %s %llx", s->cmdMap ? "found" : "NOT found", (unsigned long long)s->cmdPhys); s->note(ln);
        }
        if (s->cmdMap) {
            volatile uint32_t *c = (volatile uint32_t *)s->cmdMap->getVirtualAddress();
            bool d = false;
            for (int i = 0; i < 48; i++) if (c[i] != s->cmdLast[i]) d = true;
            if (d) {
                for (int i = 0; i < 48; i++) s->cmdLast[i] = c[i];
                for (int t = 0; t < 12; t++) {
                    if (!c[t*4] && !c[t*4+1] && !c[t*4+2] && !c[t*4+3]) continue;
                    char ln[120]; snprintf(ln, sizeof ln, "CMD[%d] %08x %08x %08x %08x", t, c[t*4], c[t*4+1], c[t*4+2], c[t*4+3]); s->note(ln);
                }
            }
        }
    }
    if (++s->ticks < 1200) {   // ~120 s at 100 ms
        uint64_t dl; clock_interval_to_deadline(100, 1000 * 1000, &dl);
        thread_call_enter_delayed(s->tc, dl);
    } else s->note("done");
}
