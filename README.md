# XHCIPeek

Diagnostic macOS kernel extension for observing an ASMedia ASM1042 xHCI controller (`1b21:1042`) while Apple's driver is active. It is a standalone `IOService` kext; it does not use Lilu or patch Apple binaries.

**Testing:** the refactored kext has been boot-tested on macOS Monterey 12.7.4; this is the only macOS version tested. No other macOS versions have been runtime-tested.

## What it records

For about two minutes after matching the controller, XHCIPeek polls its registers roughly every 100 ms. It records register changes, event-ring TRBs, and a nearby command-ring page when found. The `log` and rolling `tail` properties on the `XHCIPeek` IORegistry service contain the capture; read them with:

```sh
ioreg -l -w0 -c XHCIPeek
```

The event- and command-ring memory mappings are read-only. **The kext itself is not entirely read-only:** its BAR mapping permits MMIO writes; if an upstream PCI bridge has bus mastering disabled, it enables both bus mastering and memory decoding there. It also clears stale USBSTS.EINT, IMAN.IP, and ERDP.EHB write-one-to-clear flags while the controller is halted. It does not issue USB transfers or operate the controller as a driver.

The capture can include device addresses and physical addresses from controller registers. Treat logs as hardware diagnostics.

## Build

Requires Xcode and the upstream Acidanthera MacKernelSDK. Clone the SDK beside the project:

```sh
git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
xcodebuild -project XHCIPeek.xcodeproj -target XHCIPeek -configuration Release
```

Xcode uses MacKernelSDK for kernel headers and `libkmod`. The kext is under `build/Release/XHCIPeek.kext`; use `-configuration Debug` for a debug build. The SDK checkout is a local build dependency and is ignored by Git.
