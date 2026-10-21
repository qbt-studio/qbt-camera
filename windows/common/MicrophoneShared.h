#pragma once

// Sound path QBTCamera.exe (writer) -> "QBT Microphone" driver (reader).
//
// The driver creates this pagefile-backed section when it starts (only the kernel may create
// names in the global namespace without extra privileges); the app opens it by its Global\ name.
// Plain C types only: the header is shared by user mode and the kernel driver.
//
// Writer: put samples at Samples[n % Capacity], then publish WriteIndex = total samples written.
// Reader: keeps its own read index and plays what lies between it and WriteIndex, with a small
//         jitter buffer; stale or out-of-range indexes make it resynchronize instead of glitching.
#define QBT_MIC_SECTION_KERNEL_NAME L"\\BaseNamedObjects\\QBTMicrophoneRing"
#define QBT_MIC_SECTION_USER_NAME L"Global\\QBTMicrophoneRing"
#define QBT_MIC_MAGIC 0x4D544251UL // 'QBTM'
#define QBT_MIC_VERSION 1UL
#define QBT_MIC_SAMPLE_RATE 48000UL
#define QBT_MIC_CAPACITY 32768UL // samples, power of two (~0.68 s at 48 kHz)

typedef struct _QBT_MIC_RING {
    unsigned long Magic;
    unsigned long Version;
    unsigned long SampleRate;
    unsigned long Capacity;
    volatile long long WriteIndex;   // written by the app
    volatile long long ReaderActive; // 1 while some app records from QBT Microphone (written by the driver)
    long long Reserved[5];
    short Samples[QBT_MIC_CAPACITY]; // 48 kHz mono signed 16-bit
} QBT_MIC_RING;
