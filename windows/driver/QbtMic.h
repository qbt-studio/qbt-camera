#pragma once

// "QBT Microphone": a virtual audio capture device (PortCls / WaveRT) that turns the iPhone
// microphone, delivered by QBTCamera.exe through a shared ring buffer, into a regular Windows
// microphone every app can pick.
//
//   Topology filter               Wave filter (WaveRT)
//   +-------------+               +-------------------+
//   | Mic  0 ->  1|==============>|0 -> ADC ->       1|---> capture stream (48 kHz mono 16-bit)
//   +-------------+               +-------------------+

#include <ntifs.h>
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>

#include "../common/MicrophoneShared.h"

#define QBTMIC_POOLTAG 'MtbQ'
#define SIZEOF_ARRAY(a) (sizeof(a) / sizeof((a)[0]))

// Subdevice names: must match KSNAME_* in QbtMic.inf.
#define QBTMIC_TOPOLOGY_NAME L"TopologyQbtMic"
#define QBTMIC_WAVE_NAME L"WaveQbtMic"

enum TopologyPins { TopologyPinMicrophone = 0, TopologyPinBridge = 1 };
enum WavePins { WavePinBridge = 0, WavePinCapture = 1 };

// Kernel pool allocation for the COM-style PortCls objects (zero-initialized).
void* __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag);
void __cdecl operator delete(void* memory);
void __cdecl operator delete(void* memory, size_t size);

NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN* miniport);
NTSTATUS CreateMiniportWave(_Out_ PUNKNOWN* miniport);

// Shared ring buffer (MicRing.cpp).
NTSTATUS MicRingCreate();
void MicRingDestroy();
/// A capture stream starts or stops (resets the jitter buffer, tells the app).
void MicRingSetActive(BOOLEAN active);
/// Fills `bytes` of the cyclic capture buffer starting at `offset` with the newest sound.
/// Callable at DISPATCH_LEVEL: the ring's pages are locked in memory.
void MicRingRead(_Out_writes_bytes_(bufferSize) BYTE* buffer, ULONG bufferSize, ULONG offset, ULONG bytes);
