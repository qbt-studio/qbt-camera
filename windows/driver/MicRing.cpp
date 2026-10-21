#include "QbtMic.h"

namespace {

constexpr LONGLONG kTargetSamples = QBT_MIC_SAMPLE_RATE * 40 / 1000;  // start playing once 40 ms are buffered
constexpr LONGLONG kMaxSamples = QBT_MIC_SAMPLE_RATE * 150 / 1000;    // beyond 150 ms, catch up by dropping

HANDLE g_SectionHandle = nullptr;
PVOID g_SectionObject = nullptr;
PVOID g_SystemView = nullptr;
PMDL g_Mdl = nullptr;
QBT_MIC_RING* g_Ring = nullptr; // locked, non-paged mapping of the section: safe at DISPATCH_LEVEL

// Reader state: one capture stream at a time, touched only under that stream's spin lock.
LONGLONG g_ReadIndex = 0;
BOOLEAN g_Primed = FALSE;

/// SYSTEM and Administrators: full access. Authenticated users: map read/write, so QBTCamera.exe
/// running as the logged-on user can publish sound.
NTSTATUS BuildSecurityDescriptor(_Out_ PSECURITY_DESCRIPTOR descriptor, _Outptr_ PACL* acl) {
    const ULONG aclSize = sizeof(ACL) + 3 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG)) +
                          RtlLengthSid(SeExports->SeLocalSystemSid) + RtlLengthSid(SeExports->SeAliasAdminsSid) +
                          RtlLengthSid(SeExports->SeAuthenticatedUsersSid);
    *acl = static_cast<PACL>(ExAllocatePool2(POOL_FLAG_PAGED, aclSize, QBTMIC_POOLTAG));
    if (!*acl) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = RtlCreateSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION);
    if (NT_SUCCESS(status)) status = RtlCreateAcl(*acl, aclSize, ACL_REVISION);
    if (NT_SUCCESS(status)) status = RtlAddAccessAllowedAce(*acl, ACL_REVISION, SECTION_ALL_ACCESS, SeExports->SeLocalSystemSid);
    if (NT_SUCCESS(status)) status = RtlAddAccessAllowedAce(*acl, ACL_REVISION, SECTION_ALL_ACCESS, SeExports->SeAliasAdminsSid);
    if (NT_SUCCESS(status)) {
        status = RtlAddAccessAllowedAce(*acl, ACL_REVISION, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
                                        SeExports->SeAuthenticatedUsersSid);
    }
    if (NT_SUCCESS(status)) status = RtlSetDaclSecurityDescriptor(descriptor, TRUE, *acl, FALSE);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(*acl, QBTMIC_POOLTAG);
        *acl = nullptr;
    }
    return status;
}

} // namespace

NTSTATUS MicRingCreate() {
    if (g_Ring) return STATUS_SUCCESS; // already created on a previous start

    SECURITY_DESCRIPTOR descriptor;
    PACL acl = nullptr;
    NTSTATUS status = BuildSecurityDescriptor(&descriptor, &acl);
    if (!NT_SUCCESS(status)) return status;

    UNICODE_STRING name = RTL_CONSTANT_STRING(QBT_MIC_SECTION_KERNEL_NAME);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_OPENIF, nullptr, &descriptor);
    LARGE_INTEGER size;
    size.QuadPart = sizeof(QBT_MIC_RING);
    status = ZwCreateSection(&g_SectionHandle, SECTION_ALL_ACCESS, &attributes, &size, PAGE_READWRITE, SEC_COMMIT, nullptr);
    ExFreePoolWithTag(acl, QBTMIC_POOLTAG); // the object manager keeps its own copy
    if (!NT_SUCCESS(status)) {
        g_SectionHandle = nullptr;
        return status;
    }

    status = ObReferenceObjectByHandle(g_SectionHandle, SECTION_ALL_ACCESS, nullptr, KernelMode, &g_SectionObject, nullptr);
    SIZE_T viewSize = 0;
    if (NT_SUCCESS(status)) status = MmMapViewInSystemSpace(g_SectionObject, &g_SystemView, &viewSize);
    if (NT_SUCCESS(status)) {
        // The view itself is pageable; lock its pages so the capture timer (DISPATCH_LEVEL) can read them.
        g_Mdl = IoAllocateMdl(g_SystemView, sizeof(QBT_MIC_RING), FALSE, FALSE, nullptr);
        if (!g_Mdl) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) {
        __try {
            MmProbeAndLockPages(g_Mdl, KernelMode, IoWriteAccess);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            IoFreeMdl(g_Mdl);
            g_Mdl = nullptr;
        }
    }
    if (NT_SUCCESS(status)) {
        g_Ring = static_cast<QBT_MIC_RING*>(MmGetSystemAddressForMdlSafe(g_Mdl, NormalPagePriority | MdlMappingNoExecute));
        if (!g_Ring) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!NT_SUCCESS(status)) {
        MicRingDestroy();
        return status;
    }

    if (g_Ring->Magic != QBT_MIC_MAGIC || g_Ring->Version != QBT_MIC_VERSION) {
        RtlZeroMemory(g_Ring, sizeof(QBT_MIC_RING));
        g_Ring->Magic = QBT_MIC_MAGIC;
        g_Ring->Version = QBT_MIC_VERSION;
        g_Ring->SampleRate = QBT_MIC_SAMPLE_RATE;
        g_Ring->Capacity = QBT_MIC_CAPACITY;
    }
    return STATUS_SUCCESS;
}

void MicRingDestroy() {
    if (g_Mdl) {
        if (g_Ring) MmUnlockPages(g_Mdl); // also drops the system mapping of the locked pages
        IoFreeMdl(g_Mdl);
        g_Mdl = nullptr;
    }
    g_Ring = nullptr;
    if (g_SystemView) {
        MmUnmapViewInSystemSpace(g_SystemView);
        g_SystemView = nullptr;
    }
    if (g_SectionObject) {
        ObDereferenceObject(g_SectionObject);
        g_SectionObject = nullptr;
    }
    if (g_SectionHandle) {
        ZwClose(g_SectionHandle);
        g_SectionHandle = nullptr;
    }
}

void MicRingSetActive(BOOLEAN active) {
    g_Primed = FALSE;
    if (!g_Ring) return;
    g_ReadIndex = ReadAcquire64(&g_Ring->WriteIndex);
    InterlockedExchange64(&g_Ring->ReaderActive, active ? 1 : 0);
}

void MicRingRead(BYTE* buffer, ULONG bufferSize, ULONG offset, ULONG bytes) {
    QBT_MIC_RING* ring = g_Ring;
    LONGLONG available = 0;
    if (ring) {
        const LONGLONG written = ReadAcquire64(&ring->WriteIndex);
        available = written - g_ReadIndex;
        if (available < 0 || available > static_cast<LONGLONG>(QBT_MIC_CAPACITY)) {
            // App restarted or we were away for long: resynchronize to the newest sound.
            g_ReadIndex = written;
            available = 0;
            g_Primed = FALSE;
        }
        if (available > kMaxSamples) { // a network hiccup or clock drift built up a backlog
            g_ReadIndex = written - kTargetSamples;
            available = kTargetSamples;
        }
        if (!g_Primed && available >= kTargetSamples) g_Primed = TRUE;
    }

    const ULONG samples = bytes / sizeof(SHORT);
    for (ULONG i = 0; i < samples; ++i) {
        SHORT value = 0;
        if (g_Primed && available > 0) {
            value = ring->Samples[g_ReadIndex & (QBT_MIC_CAPACITY - 1)];
            ++g_ReadIndex;
            --available;
        } else if (g_Primed) {
            g_Primed = FALSE; // underrun: rebuffer before playing again
        }
        // Buffer size and offset are multiples of the 2-byte frame, so a sample never straddles the wrap.
        *reinterpret_cast<SHORT*>(buffer + (offset + i * sizeof(SHORT)) % bufferSize) = value;
    }
}
