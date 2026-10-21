#include "QbtMic.h"

namespace {

constexpr ULONG kChannels = 1;
constexpr ULONG kBitsPerSample = 16;
constexpr ULONG kBlockAlign = kChannels * kBitsPerSample / 8;
constexpr ULONG kBytesPerSecond = QBT_MIC_SAMPLE_RATE * kBlockAlign;
constexpr LONGLONG kTimerPeriod = 10'000; // 1 ms in 100 ns units

// Exactly what the phone sends (48 kHz mono 16-bit): no resampling anywhere on the way.
KSDATARANGE_AUDIO g_CaptureRanges[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
        },
        kChannels,
        kBitsPerSample,
        kBitsPerSample,
        QBT_MIC_SAMPLE_RATE,
        QBT_MIC_SAMPLE_RATE,
    },
};

PKSDATARANGE g_CaptureRangePointers[] = {PKSDATARANGE(&g_CaptureRanges[0])};

KSDATARANGE g_BridgeRanges[] = {
    {
        sizeof(KSDATARANGE), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE),
    },
};

PKSDATARANGE g_BridgeRangePointers[] = {&g_BridgeRanges[0]};

PCPIN_DESCRIPTOR g_Pins[] = {
    // WavePinBridge: from the topology filter.
    {0, 0, 0, nullptr,
     {0, nullptr, 0, nullptr, SIZEOF_ARRAY(g_BridgeRangePointers), g_BridgeRangePointers, KSPIN_DATAFLOW_IN,
      KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
    // WavePinCapture: the stream apps record from. One at a time: the Windows audio engine shares it.
    {1, 1, 0, nullptr,
     {0, nullptr, 0, nullptr, SIZEOF_ARRAY(g_CaptureRangePointers), g_CaptureRangePointers, KSPIN_DATAFLOW_OUT,
      KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, &KSAUDFNAME_RECORDING_CONTROL, 0}},
};

PCNODE_DESCRIPTOR g_Nodes[] = {
    {0, nullptr, &KSNODETYPE_ADC, nullptr},
};

PCCONNECTION_DESCRIPTOR g_Connections[] = {
    {PCFILTER_NODE, WavePinBridge, 0, 1},  // bridge -> ADC input
    {0, 0, PCFILTER_NODE, WavePinCapture}, // ADC output -> capture pin
};

PCFILTER_DESCRIPTOR g_FilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_Pins), g_Pins,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(g_Nodes), g_Nodes,
    SIZEOF_ARRAY(g_Connections), g_Connections,
    0, nullptr,
};

bool IsSupportedFormat(PKSDATAFORMAT format) {
    if (!format || format->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) return false;
    if (!IsEqualGUIDAligned(format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
        return false;
    const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(format + 1);
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) return false;
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (!IsEqualGUIDAligned(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) return false;
    } else if (wave->wFormatTag != WAVE_FORMAT_PCM) {
        return false;
    }
    return wave->nChannels == kChannels && wave->nSamplesPerSec == QBT_MIC_SAMPLE_RATE &&
           wave->wBitsPerSample == kBitsPerSample;
}

struct NotificationEntry {
    LIST_ENTRY link;
    PKEVENT event;
};

class CMiniportWave;

// One capture stream. A 1 ms high-resolution timer plays the role of the "hardware": it advances
// the position by the elapsed time and fills the cyclic buffer with sound from the ring.
class CMiniportWaveStream : public IMiniportWaveRTStreamNotification, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveStream);
    ~CMiniportWaveStream();

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;

    NTSTATUS Init(_In_ PPORTWAVERTSTREAM portStream);

private:
    static EXT_CALLBACK TimerCallback;
    void UpdatePosition(LONGLONG qpcNow); // m_Lock held
    NTSTATUS AllocateBuffer(ULONG requestedSize, PMDL* mdl, ULONG* actualSize, ULONG* offset, MEMORY_CACHING_TYPE* cacheType);

    PPORTWAVERTSTREAM m_PortStream;
    PEX_TIMER m_Timer;
    KSPIN_LOCK m_Lock;
    LIST_ENTRY m_Notifications;
    KSSTATE m_State;
    BYTE* m_Buffer;
    ULONG m_BufferSize;
    ULONG m_BytesPerNotification;
    ULONGLONG m_NextNotificationAt;
    ULONGLONG m_LinearBytes; // total bytes "captured" since STOP
    ULONGLONG m_RunStartBytes;
    LONGLONG m_RunStartQpc;
    LONGLONG m_QpcFrequency;
};

class CMiniportWave : public IMiniportWaveRT, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWave);
    ~CMiniportWave() {
        if (m_Port) m_Port->Release();
    }

    IMP_IMiniportWaveRT;

private:
    PPORTWAVERT m_Port;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// CMiniportWave

STDMETHODIMP_(NTSTATUS) CMiniportWave::NonDelegatingQueryInterface(_In_ REFIID interfaceId, _COM_Outptr_ PVOID* object) {
    if (IsEqualGUIDAligned(interfaceId, IID_IUnknown)) {
        *object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    } else if (IsEqualGUIDAligned(interfaceId, IID_IMiniport)) {
        *object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(interfaceId, IID_IMiniportWaveRT)) {
        *object = PVOID(PMINIPORTWAVERT(this));
    } else {
        *object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::Init(_In_ PUNKNOWN, _In_ PRESOURCELIST, _In_ PPORTWAVERT port) {
    m_Port = port;
    m_Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* description) {
    *description = &g_FilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::DataRangeIntersection(_In_ ULONG, _In_ PKSDATARANGE, _In_ PKSDATARANGE, _In_ ULONG,
                                                              _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID,
                                                              _Out_ PULONG) {
    return STATUS_NOT_IMPLEMENTED; // PortCls' default intersection handles our single PCM range
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION description) {
    // No real DMA hardware; describe a bus-master device like the Microsoft virtual audio samples do.
    RtlZeroMemory(description, sizeof(DEVICE_DESCRIPTION));
    description->Master = TRUE;
    description->ScatterGather = TRUE;
    description->Dma32BitAddresses = TRUE;
    description->InterfaceType = PCIBus;
    description->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::NewStream(_Out_ PMINIPORTWAVERTSTREAM* stream, _In_ PPORTWAVERTSTREAM portStream,
                                                 _In_ ULONG pin, _In_ BOOLEAN capture, _In_ PKSDATAFORMAT format) {
    *stream = nullptr;
    if (pin != WavePinCapture || !capture) return STATUS_INVALID_PARAMETER;
    if (!IsSupportedFormat(format)) return STATUS_NO_MATCH;

    auto* newStream = new (POOL_FLAG_NON_PAGED, QBTMIC_POOLTAG) CMiniportWaveStream(nullptr);
    if (!newStream) return STATUS_INSUFFICIENT_RESOURCES;
    newStream->AddRef(); // this reference goes to PortCls
    const NTSTATUS status = newStream->Init(portStream);
    if (!NT_SUCCESS(status)) {
        newStream->Release();
        return status;
    }
    *stream = PMINIPORTWAVERTSTREAM(newStream);
    return STATUS_SUCCESS;
}

NTSTATUS CreateMiniportWave(_Out_ PUNKNOWN* miniport) {
    auto* wave = new (POOL_FLAG_NON_PAGED, QBTMIC_POOLTAG) CMiniportWave(nullptr);
    if (!wave) return STATUS_INSUFFICIENT_RESOURCES;
    *miniport = PUNKNOWN(PMINIPORTWAVERT(wave));
    (*miniport)->AddRef();
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// CMiniportWaveStream

NTSTATUS CMiniportWaveStream::Init(_In_ PPORTWAVERTSTREAM portStream) {
    m_PortStream = portStream;
    m_PortStream->AddRef();
    m_State = KSSTATE_STOP;
    KeInitializeSpinLock(&m_Lock);
    InitializeListHead(&m_Notifications);
    LARGE_INTEGER frequency;
    KeQueryPerformanceCounter(&frequency);
    m_QpcFrequency = frequency.QuadPart;
    m_Timer = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
    return m_Timer ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

CMiniportWaveStream::~CMiniportWaveStream() {
    if (m_Timer) ExDeleteTimer(m_Timer, TRUE, TRUE, nullptr); // cancels and waits for a running callback
    while (!IsListEmpty(&m_Notifications)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&m_Notifications), NotificationEntry, link), QBTMIC_POOLTAG);
    }
    if (m_State != KSSTATE_STOP) MicRingSetActive(FALSE);
    if (m_PortStream) m_PortStream->Release();
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::NonDelegatingQueryInterface(_In_ REFIID interfaceId, _COM_Outptr_ PVOID* object) {
    if (IsEqualGUIDAligned(interfaceId, IID_IUnknown)) {
        *object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    } else if (IsEqualGUIDAligned(interfaceId, IID_IMiniportWaveRTStream)) {
        *object = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUIDAligned(interfaceId, IID_IMiniportWaveRTStreamNotification)) {
        *object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    } else {
        *object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*object)->AddRef();
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveStream::AllocateBuffer(ULONG requestedSize, PMDL* mdl, ULONG* actualSize, ULONG* offset,
                                             MEMORY_CACHING_TYPE* cacheType) {
    requestedSize -= requestedSize % kBlockAlign;
    if (requestedSize == 0) return STATUS_UNSUCCESSFUL;

    PHYSICAL_ADDRESS highest;
    highest.QuadPart = MAXULONG;
    PMDL buffer = m_PortStream->AllocatePagesForMdl(highest, requestedSize);
    if (!buffer) return STATUS_INSUFFICIENT_RESOURCES;
    auto* mapped = static_cast<BYTE*>(m_PortStream->MapAllocatedPages(buffer, MmCached));
    if (!mapped) {
        m_PortStream->FreePagesFromMdl(buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(mapped, requestedSize);

    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Buffer = mapped;
    m_BufferSize = requestedSize;
    KeReleaseSpinLock(&m_Lock, irql);

    *mdl = buffer;
    *actualSize = requestedSize;
    *offset = 0;
    *cacheType = MmCached;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::AllocateAudioBuffer(_In_ ULONG requestedSize, _Out_ PMDL* mdl, _Out_ ULONG* actualSize,
                                                                 _Out_ ULONG* offset, _Out_ MEMORY_CACHING_TYPE* cacheType) {
    m_BytesPerNotification = 0; // polling mode: the engine calls GetPosition
    return AllocateBuffer(requestedSize, mdl, actualSize, offset, cacheType);
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::AllocateBufferWithNotification(_In_ ULONG notificationCount, _In_ ULONG requestedSize,
                                                                            _Out_ PMDL* mdl, _Out_ ULONG* actualSize,
                                                                            _Out_ ULONG* offset, _Out_ MEMORY_CACHING_TYPE* cacheType) {
    if (notificationCount == 0 || requestedSize % notificationCount != 0) return STATUS_INVALID_PARAMETER;
    const NTSTATUS status = AllocateBuffer(requestedSize, mdl, actualSize, offset, cacheType);
    if (NT_SUCCESS(status)) {
        m_BytesPerNotification = *actualSize / notificationCount;
        m_NextNotificationAt = m_LinearBytes + m_BytesPerNotification;
    }
    return status;
}

STDMETHODIMP_(VOID) CMiniportWaveStream::FreeAudioBuffer(_In_opt_ PMDL mdl, _In_ ULONG) {
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    BYTE* buffer = m_Buffer;
    m_Buffer = nullptr;
    m_BufferSize = 0;
    KeReleaseSpinLock(&m_Lock, irql);
    if (mdl) {
        if (buffer) m_PortStream->UnmapAllocatedPages(buffer, mdl);
        m_PortStream->FreePagesFromMdl(mdl);
    }
}

STDMETHODIMP_(VOID) CMiniportWaveStream::FreeBufferWithNotification(_In_ PMDL mdl, _In_ ULONG size) {
    FreeAudioBuffer(mdl, size);
    m_BytesPerNotification = 0;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::RegisterNotificationEvent(_In_ PKEVENT event) {
    auto* entry = static_cast<NotificationEntry*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(NotificationEntry), QBTMIC_POOLTAG));
    if (!entry) return STATUS_INSUFFICIENT_RESOURCES;
    entry->event = event;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    InsertTailList(&m_Notifications, &entry->link);
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::UnregisterNotificationEvent(_In_ PKEVENT event) {
    NotificationEntry* found = nullptr;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    for (PLIST_ENTRY link = m_Notifications.Flink; link != &m_Notifications; link = link->Flink) {
        auto* entry = CONTAINING_RECORD(link, NotificationEntry, link);
        if (entry->event == event) {
            RemoveEntryList(link);
            found = entry;
            break;
        }
    }
    KeReleaseSpinLock(&m_Lock, irql);
    if (!found) return STATUS_NOT_FOUND;
    ExFreePoolWithTag(found, QBTMIC_POOLTAG);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::SetFormat(_In_ PKSDATAFORMAT) {
    return STATUS_NOT_SUPPORTED; // fixed format for the stream's lifetime
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::SetState(_In_ KSSTATE state) {
    const KSSTATE previous = m_State;
    if (previous == KSSTATE_RUN && state != KSSTATE_RUN) {
        // Stop the "hardware" first; the callback takes m_Lock, so not while holding it.
        ExCancelTimer(m_Timer, nullptr);
        KeFlushQueuedDpcs();
    }

    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    const LONGLONG now = KeQueryPerformanceCounter(nullptr).QuadPart;
    switch (state) {
    case KSSTATE_RUN:
        if (previous != KSSTATE_RUN) {
            if (previous == KSSTATE_STOP || previous == KSSTATE_ACQUIRE) MicRingSetActive(TRUE);
            m_RunStartQpc = now;
            m_RunStartBytes = m_LinearBytes;
        }
        break;
    case KSSTATE_PAUSE:
        if (previous == KSSTATE_ACQUIRE || previous == KSSTATE_STOP) MicRingSetActive(TRUE);
        break;
    case KSSTATE_STOP:
        m_LinearBytes = 0;
        m_NextNotificationAt = m_BytesPerNotification;
        MicRingSetActive(FALSE);
        break;
    default:
        break;
    }
    m_State = state;
    KeReleaseSpinLock(&m_Lock, irql);

    if (state == KSSTATE_RUN && previous != KSSTATE_RUN) ExSetTimer(m_Timer, -kTimerPeriod, kTimerPeriod, nullptr);
    return STATUS_SUCCESS;
}

void CMiniportWaveStream::UpdatePosition(LONGLONG qpcNow) {
    if (m_State != KSSTATE_RUN || !m_Buffer || m_QpcFrequency == 0) return;
    // Position from the QPC clock since RUN: exact and drift-free over hours.
    ULONGLONG target = m_RunStartBytes + static_cast<ULONGLONG>((qpcNow - m_RunStartQpc) * kBytesPerSecond / m_QpcFrequency);
    target -= target % kBlockAlign;
    if (target <= m_LinearBytes) return;
    ULONGLONG delta = target - m_LinearBytes;
    if (delta > m_BufferSize) { // we were starved for longer than a whole buffer: skip ahead
        m_LinearBytes = target - m_BufferSize;
        delta = m_BufferSize;
    }
    MicRingRead(m_Buffer, m_BufferSize, static_cast<ULONG>(m_LinearBytes % m_BufferSize), static_cast<ULONG>(delta));
    m_LinearBytes += delta;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::GetPosition(_Out_ PKSAUDIO_POSITION position) {
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    UpdatePosition(KeQueryPerformanceCounter(nullptr).QuadPart);
    const ULONGLONG offset = m_BufferSize ? m_LinearBytes % m_BufferSize : 0;
    position->PlayOffset = offset;
    position->WriteOffset = offset;
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}

void CMiniportWaveStream::TimerCallback(PEX_TIMER, PVOID context) {
    auto* self = static_cast<CMiniportWaveStream*>(context);
    KeAcquireSpinLockAtDpcLevel(&self->m_Lock);
    self->UpdatePosition(KeQueryPerformanceCounter(nullptr).QuadPart);
    if (self->m_BytesPerNotification && self->m_LinearBytes >= self->m_NextNotificationAt) {
        // A notification period's worth of sound is ready: wake the audio engine.
        while (self->m_NextNotificationAt <= self->m_LinearBytes) self->m_NextNotificationAt += self->m_BytesPerNotification;
        for (PLIST_ENTRY link = self->m_Notifications.Flink; link != &self->m_Notifications; link = link->Flink) {
            KeSetEvent(CONTAINING_RECORD(link, NotificationEntry, link)->event, 0, FALSE);
        }
    }
    KeReleaseSpinLockFromDpcLevel(&self->m_Lock);
}

STDMETHODIMP_(VOID) CMiniportWaveStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY* latency) {
    latency->ChipsetDelay = 0;
    latency->CodecDelay = 0;
    latency->FifoSize = 0;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER*) {
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER*) {
    return STATUS_NOT_SUPPORTED;
}
