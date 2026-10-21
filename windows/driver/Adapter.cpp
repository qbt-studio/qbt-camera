#include "QbtMic.h"

namespace {

using PortClassUnload = void (*)(PDRIVER_OBJECT);
PortClassUnload g_PortClassUnload = nullptr;

/// Creates a port of `portClass`, binds our miniport to it and registers it as subdevice `name`.
NTSTATUS InstallSubdevice(PDEVICE_OBJECT device, PIRP irp, PWSTR name, REFGUID portClass,
                          NTSTATUS (*createMiniport)(PUNKNOWN*), PRESOURCELIST resources, PUNKNOWN* port) {
    PPORT newPort = nullptr;
    PUNKNOWN miniport = nullptr;
    NTSTATUS status = PcNewPort(&newPort, portClass);
    if (NT_SUCCESS(status)) status = createMiniport(&miniport);
    if (NT_SUCCESS(status)) status = newPort->Init(device, irp, miniport, nullptr, resources);
    if (NT_SUCCESS(status)) status = PcRegisterSubdevice(device, name, newPort);
    if (miniport) miniport->Release(); // the port holds its own reference
    if (NT_SUCCESS(status)) {
        *port = newPort;
    } else if (newPort) {
        newPort->Release();
    }
    return status;
}

NTSTATUS StartDevice(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources) {
    NTSTATUS status = MicRingCreate();
    if (!NT_SUCCESS(status)) return status;

    PUNKNOWN topology = nullptr;
    PUNKNOWN wave = nullptr;
    status = InstallSubdevice(device, irp, const_cast<PWSTR>(QBTMIC_TOPOLOGY_NAME), CLSID_PortTopology,
                              CreateMiniportTopology, resources, &topology);
    if (NT_SUCCESS(status)) {
        status = InstallSubdevice(device, irp, const_cast<PWSTR>(QBTMIC_WAVE_NAME), CLSID_PortWaveRT,
                                  CreateMiniportWave, resources, &wave);
    }
    // Tells the audio stack that the microphone bridge of the topology feeds the wave filter.
    if (NT_SUCCESS(status)) status = PcRegisterPhysicalConnection(device, topology, TopologyPinBridge, wave, WavePinBridge);
    if (topology) topology->Release();
    if (wave) wave->Release();
    return status;
}

NTSTATUS AddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT physicalDevice) {
    return PcAddAdapterDevice(driver, physicalDevice, PCPFNSTARTDEVICE(StartDevice), 2 /* subdevices */, 0);
}

void DriverUnload(PDRIVER_OBJECT driver) {
    MicRingDestroy();
    if (g_PortClassUnload) g_PortClassUnload(driver);
}

} // namespace

void* __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag) {
    return ExAllocatePool2(flags, size, tag);
}

void __cdecl operator delete(void* memory) {
    if (memory) ExFreePool(memory);
}

void __cdecl operator delete(void* memory, size_t) {
    if (memory) ExFreePool(memory);
}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    const NTSTATUS status = PcInitializeAdapterDriver(driver, registryPath, PDRIVER_ADD_DEVICE(AddDevice));
    if (!NT_SUCCESS(status)) return status;
    // PortCls installs its own unload; run ours (free the ring) before it.
    g_PortClassUnload = driver->DriverUnload;
    driver->DriverUnload = DriverUnload;
    return STATUS_SUCCESS;
}
