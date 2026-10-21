#include "QbtMic.h"

namespace {

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
    // TopologyPinMicrophone: the "physical" microphone. Its category makes Windows call the endpoint a microphone.
    {0, 0, 0, nullptr,
     {0, nullptr, 0, nullptr, SIZEOF_ARRAY(g_BridgeRangePointers), g_BridgeRangePointers, KSPIN_DATAFLOW_IN,
      KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, nullptr, 0}},
    // TopologyPinBridge: connects to the wave filter.
    {0, 0, 0, nullptr,
     {0, nullptr, 0, nullptr, SIZEOF_ARRAY(g_BridgeRangePointers), g_BridgeRangePointers, KSPIN_DATAFLOW_OUT,
      KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
};

PCCONNECTION_DESCRIPTOR g_Connections[] = {
    {PCFILTER_NODE, TopologyPinMicrophone, PCFILTER_NODE, TopologyPinBridge},
};

PCFILTER_DESCRIPTOR g_FilterDescriptor = {
    0,                            // Version
    nullptr,                      // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),     // PinSize
    SIZEOF_ARRAY(g_Pins),         // PinCount
    g_Pins,                       // Pins
    sizeof(PCNODE_DESCRIPTOR),    // NodeSize
    0,                            // NodeCount: volume is handled by Windows in software
    nullptr,                      // Nodes
    SIZEOF_ARRAY(g_Connections),  // ConnectionCount
    g_Connections,                // Connections
    0,                            // CategoryCount
    nullptr,                      // Categories: defaults
};

class CMiniportTopology : public IMiniportTopology, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology() {
        if (m_Port) m_Port->Release();
    }

    IMP_IMiniportTopology;

private:
    PPORTTOPOLOGY m_Port;
};

} // namespace

STDMETHODIMP_(NTSTATUS) CMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID interfaceId, _COM_Outptr_ PVOID* object) {
    if (IsEqualGUIDAligned(interfaceId, IID_IUnknown)) {
        *object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    } else if (IsEqualGUIDAligned(interfaceId, IID_IMiniport)) {
        *object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(interfaceId, IID_IMiniportTopology)) {
        *object = PVOID(PMINIPORTTOPOLOGY(this));
    } else {
        *object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::Init(_In_ PUNKNOWN, _In_ PRESOURCELIST, _In_ PPORTTOPOLOGY port) {
    m_Port = port;
    m_Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* description) {
    *description = &g_FilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::DataRangeIntersection(_In_ ULONG, _In_ PKSDATARANGE, _In_ PKSDATARANGE,
                                                                  _In_ ULONG, _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID,
                                                                  _Out_ PULONG) {
    return STATUS_NOT_IMPLEMENTED; // let PortCls do the default intersection
}

NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN* miniport) {
    auto* topology = new (POOL_FLAG_NON_PAGED, QBTMIC_POOLTAG) CMiniportTopology(nullptr);
    if (!topology) return STATUS_INSUFFICIENT_RESOURCES;
    *miniport = PUNKNOWN(PMINIPORTTOPOLOGY(topology));
    (*miniport)->AddRef();
    return STATUS_SUCCESS;
}
