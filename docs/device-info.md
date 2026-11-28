# DeviceInfo: the model, the memory, the GPU, the OS version and the locale

`lib/DeviceInfo.rae` says what a program runs on. A game picks its starting graphics tier from it,
and analytics sends it with a session. (`lib/Device.rae` is something else: the input a device has
right now, such as touch, pointer and hover, which can change while the program runs.)

```rae
import DeviceInfo

let model: String = DeviceInfo.deviceModel()        # "iPhone16,1", "Pixel 8", "Mac15,9"
let memoryMb: Int = DeviceInfo.physicalMemoryMb()   # 8192; -1 when not known
let gpu: String = DeviceInfo.gpuName()              # "Adreno (TM) 740" on Android, else ""
let version: String = DeviceInfo.osVersion()        # "18.1", "15", "26.1"
let tag: String = DeviceInfo.locale()               # "en-FI"
```

A platform with no answer gives `""` (or `-1` for the memory). All five can be read before the
first window. Each one is a single quick system call, except `gpuName` on Android, which asks
Vulkan once (a few milliseconds) and then keeps the name. The values do not change while the
program runs: read them once at the start and keep them on the app's state.

## Where each value comes from

| | iOS | macOS | Android | Linux, BSD | browser, Windows |
|---|---|---|---|---|---|
| `deviceModel` | sysctl `hw.machine` ("iPhone16,1"); in the simulator `SIMULATOR_MODEL_IDENTIFIER` | sysctl `hw.model` ("Mac15,9") | `ro.product.model` ("Pixel 8"), Java's `Build.MODEL` | `/sys/devices/virtual/dmi/id/product_name` | "" |
| `physicalMemoryMb` | sysctl `hw.memsize` | sysctl `hw.memsize` | sysconf pages x page size (`MemTotal`, Java's `ActivityManager.totalMem`) | sysconf | -1 |
| `gpuName` | "" | "" | the first Vulkan device's `deviceName` | "" | "" |
| `osVersion` | sysctl `kern.osproductversion` ("18.1") | the same ("26.1") | `ro.build.version.release` ("15"), Java's `Build.VERSION.RELEASE` | `/etc/os-release` `VERSION_ID` ("24.04") | "" |
| `locale` | CoreFoundation's first preferred language ("en-FI") | the same | SDL's first preferred locale, Java's `Locale.getDefault()` ("en-FI") | `LC_ALL`, `LC_MESSAGES` or `LANG` ("en_US.UTF-8" is "en-US") | "" |

On Android the GPU's name is the Vulkan device's: "Adreno (TM) 740" or "Mali-G57" on a phone. An
emulator reports the host's GPU through its Vulkan pass-through ("Apple M1 Max" on a Mac). A phone
without Vulkan gives "".

The C side is `compiler/runtime/runtime_device_info.h`, a header included only by programs that
import the module. Each function there makes one platform call (`sysctlbyname`,
`__system_property_get`, `sysconf`, `CFLocaleCopyPreferredLanguages`, `SDL_GetPreferredLocales`).
The Vulkan query is the one exception, a fixed sequence with no choices: load `libvulkan.so`,
create an instance, list the devices, read the first one's properties, then destroy the instance.
Which name to ask for, the units and every fallback are written in Rae.

## Checked

Case 1712 prints whether each value is known and checks the parsing (the POSIX locale and
os-release). `RAE_SHOW_DEVICE_INFO=1` also prints the values themselves:

- on a MacBook Pro: model "MacBookPro18,2", memory 65536 MiB, GPU "", OS "27.0", locale "en-FI";
- on an Android 15 emulator: model "sdk_gphone64_arm64", memory 2472 MiB (`MemTotal`
  2531992 kB), GPU "Apple M1 Max" (the host's GPU, passed through), OS "15", locale "en-US".
