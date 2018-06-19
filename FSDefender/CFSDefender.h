#pragma once

#include "FSDCommonDefs.h"
#include "CFilter.h"
#include "CFSDCommunicationPort.h"
#include "AutoPtr.h"
#include "FSDAtomicQueue.h"
#include "FSDStringUtils.h"

struct IrpOperationItem;

class CFSDefender
{
public:
    CFSDefender();
    ~CFSDefender();

    NTSTATUS Initialize(
        PDRIVER_OBJECT          pDriverObject
    );

    void Close();

    LPCWSTR GetScanDirectoryName() const
    {
        return m_wszScanPath.LetPtr();
    }

    NTSTATUS ConnectClient(PFLT_PORT pClientPort);

    void DisconnectClient(PFLT_PORT pClientPort);

    NTSTATUS HandleNewMessage(
        IN  PVOID pvInputBuffer,
        IN  ULONG uInputBufferLength,
        OUT PVOID pvOutputBuffer,
        IN  ULONG uOutputBufferLength,
        OUT PULONG puReturnOutputBufferLength
    );


    NTSTATUS ProcessPreIrp(PFLT_CALLBACK_DATA pData);

public:
    static NTSTATUS OnConnect(PVOID pvContext, PFLT_PORT pClientPort)
    {
        CFSDefender* pDefender = static_cast<CFSDefender*>(pvContext);
        RETURN_IF_FAILED_ALLOC(pDefender);

        return pDefender->ConnectClient(pClientPort);
    }

    static void OnDisconnect(PVOID pvContext, PFLT_PORT pClientPort)
    {
        CFSDefender* pDefender = static_cast<CFSDefender*>(pvContext);
        if (!pDefender) 
        {
            return;
        }

        return pDefender->DisconnectClient(pClientPort);
    }

    static NTSTATUS OnNewMessage(
        IN  PVOID pvContext,
        IN  PVOID pvInputBuffer,
        IN  ULONG uInputBufferLength,
        OUT PVOID pvOutputBuffer,
        IN  ULONG uOutputBufferLength,
        OUT PULONG puReturnOutputBufferLength)
    {
        CFSDefender* pDefender = static_cast<CFSDefender*>(pvContext);
        RETURN_IF_FAILED_ALLOC(pDefender);

        return pDefender->HandleNewMessage(pvInputBuffer, uInputBufferLength, pvOutputBuffer, uOutputBufferLength, puReturnOutputBufferLength);
    }

    static NTSTATUS
    FSDInstanceSetup(
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
        _In_ DEVICE_TYPE VolumeDeviceType,
        _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    );

    static VOID
    FSDInstanceTeardownStart(
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
    );

    static VOID
    FSDInstanceTeardownComplete(
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
    );

    static NTSTATUS
    FSDUnload(
        _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

    static NTSTATUS
    FSDInstanceQueryTeardown(
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    );

    static ULONG_PTR OperationStatusCtx;

    static FLT_PREOP_CALLBACK_STATUS
    FSDPreOperation(
        _Inout_ PFLT_CALLBACK_DATA Data,
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

    static VOID
    FSDOperationStatusCallback(
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _In_ PFLT_IO_PARAMETER_BLOCK ParameterSnapshot,
        _In_ NTSTATUS OperationStatus,
        _In_ PVOID RequesterContext
    );

    static FLT_POSTOP_CALLBACK_STATUS
    FSDPostOperation(
        _Inout_ PFLT_CALLBACK_DATA Data,
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _In_opt_ PVOID CompletionContext,
        _In_ FLT_POST_OPERATION_FLAGS Flags
    );

    static FLT_PREOP_CALLBACK_STATUS
    FSDPreOperationNoPostOperation(
        _Inout_ PFLT_CALLBACK_DATA Data,
        _In_ PCFLT_RELATED_OBJECTS FltObjects,
        _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    );

    static BOOLEAN
    FSDDoRequestOperationStatus(
        _In_ PFLT_CALLBACK_DATA Data
    );

private:
    static bool IsFilenameForScan(UNICODE_STRING);
    static void FillOperationDescription(FSD_OPERATION_DESCRIPTION* pOpDescription, IrpOperationItem* pIrpOp);

private:
    bool                            m_fClosed;

    CAutoPtr<CFilter>               m_pFilter;
    CAutoPtr<CFSDCommunicationPort> m_pPort;
    CAutoStringW                    m_wszScanPath;

    bool                            m_fSniffer;

    CAtomicQueue<IrpOperationItem>  m_aIrpOpsQueue;
    IrpOperationItem*               m_pItemsReadyForSend;
};

struct IrpOperationItem : public SingleListItem
{
    ULONG               m_uIrpMajorCode;
    ULONG               m_uIrpMinorCode;
    ULONG               m_uPid;
    double              m_dWriteEntropy;

    WCHAR               m_wszFileExtention[MAX_FILE_EXTENTION_LENGTH];

    size_t              m_cbFileName;
    CAutoArrayPtr<BYTE> m_pFileName;
	bool				m_checkForDelete;

    IrpOperationItem(ULONG uIrpMajorCode, ULONG uIrpMinorCode, ULONG uPid, bool checkForDelete)
        : m_uIrpMajorCode(uIrpMajorCode)
        , m_uIrpMinorCode(uIrpMinorCode)
        , m_uPid(uPid)
        , m_cbFileName(0)
		, m_checkForDelete(checkForDelete)
    {}

    NTSTATUS SetFileName(LPCWSTR wszFileName, size_t cbFileName)
    {
        NTSTATUS hr = S_OK;

        CAutoArrayPtr<BYTE> pFileName = new BYTE[cbFileName];
        RETURN_IF_FAILED_ALLOC(pFileName);

        hr = CopyStringW((LPWSTR)pFileName.LetPtr(), wszFileName, cbFileName);
        RETURN_IF_FAILED(hr);

        m_pFileName.Swap(pFileName);
        m_cbFileName = cbFileName;

        return S_OK;
    }

    NTSTATUS SetFileExtention(LPCWSTR wszFileExtention, size_t cbFileExtention)
    {
        NTSTATUS hr = S_OK;

        if (!wszFileExtention || (cbFileExtention >= sizeof(m_wszFileExtention)))
        {
            m_wszFileExtention[0] = 0;
            return S_OK;
        }

        ASSERT(cbFileExtention < sizeof(m_wszFileExtention));

        hr = CopyStringW(m_wszFileExtention, wszFileExtention, sizeof(m_wszFileExtention));
        RETURN_IF_FAILED(hr);

        return S_OK;
    }

    size_t PureSize() const
    {
        return sizeof(IrpOperationItem) - sizeof(m_pFileName) + m_cbFileName;
    }
};