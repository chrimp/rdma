#ifndef NDSESSION_HPP
#define NDSESSION_HPP
#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ndsupport.h>
#include <variant>
#include <iostream>

class NDSessionBase {
    public:
    void CheckForOPs() {
        ND2_RESULT ndRes = WaitForCompletion(ND_CQ_NOTIFY_ANY, false);
        if (ndRes.Status != ND_PENDING) {
            std::cout << "There are pending operations: " << ndRes.RequestType << std::endl;
        } else {
            std::cout << "There are no pending operations." << std::endl;
        }
    }

    protected:
    IND2Adapter *m_pAdapter;
    IND2MemoryRegion *m_pMr;
    IND2CompletionQueue *m_pCq;
    IND2QueuePair *m_pQp;
    IND2Connector *m_pConnector;
    HANDLE m_hAdapterFile;
    DWORD m_Buf_Len;
    void* m_Buf;
    IND2MemoryWindow *m_pMw;
    OVERLAPPED m_Ov;

    size_t m_MaxPerTransfer = 1500;

    protected:
    NDSessionBase();
    ~NDSessionBase();

    // Initialize the adapter with the given ipv4 addr
    bool Initialize(char* localAddr);

    HRESULT CreateMW();
    HRESULT InvalidateMW();
    
    ND2_ADAPTER_INFO GetAdapterInfo();

    HRESULT CreateMR();
    HRESULT RegisterDataBuffer(DWORD bufferLength, ULONG type);
    HRESULT RegisterDataBuffer(void *pBuffer, DWORD bufferLength, ULONG type);
    HRESULT CreateCQ(DWORD depth);
    HRESULT CreateCQ(IND2CompletionQueue **pCq, DWORD depth);
    HRESULT CreateConnector();
    HRESULT CreateQP(DWORD queueDepth, DWORD nSge, DWORD inlineDataSize = 0);
    HRESULT CreateQP(DWORD receiveQueueDepth, DWORD initiatorQueueDepth, DWORD maxReceiveRequestSge, DWORD maxInitiatorRequestSge);

    void ClearOPs();
    
    void DisconnectConnector();
    void DeregisterMemory();

    HRESULT GetResult();

    DWORD PrepareSge(ND2_SGE *pSge, const DWORD nSge, char* pBuf, ULONG buffSize, ULONG headerSize, UINT32 memoryToken);

    HRESULT PostReceive(const ND2_SGE* Sge, const DWORD nSge, void *requestContext = nullptr);

    HRESULT Send(const ND2_SGE* Sge, const ULONG nSge, ULONG flags, void* requestContext = nullptr);
    HRESULT Write(const ND2_SGE* Sge, const ULONG nSge, UINT64 remoteAddr, UINT32 remoteToken, DWORD flags, void *requestContext = nullptr);
    HRESULT Read(const ND2_SGE* Sge, const ULONG nSge, UINT64 remoteAddr, UINT32 remoteToken, DWORD flags, void *requestContext = nullptr);

    void WaitForEventNotification(ULONG notifyFlag);
    
    ND2_RESULT WaitForCompletion(ULONG notifyFlag, bool bBlocking = true);
    HRESULT WaitForCompletion();

    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);

    std::variant<HRESULT, ND2_RESULT> Bind(DWORD bufferLength, ULONG type, void *context = nullptr);
    std::variant<HRESULT, ND2_RESULT> Bind(const void *pBuf, DWORD BufferLength, ULONG type, void *context = nullptr);

    void Shutdown();

    HRESULT FlushQP();

    HRESULT Reject(const VOID *pPrivateData, DWORD cbPrivateData);
};

class NDSessionServerBase : public NDSessionBase {
    protected:
    IND2Listener *m_pListen;

    public:
    NDSessionServerBase();
    ~NDSessionServerBase();

    HRESULT CreateListener();
    HRESULT Listen(const char *localAddr);
    HRESULT GetConnectionRequest();
    HRESULT Accept(DWORD inboundReadLimit, DWORD outboundReadLimit, const void *pPrivateData, DWORD cbPrivateData);
};

class NDSessionClientBase : public NDSessionBase {
    public:
    HRESULT Connect(const char *localAddr, const char *remoteAddr, DWORD inboundReadLimit, DWORD outboundReadLimit, const void *pPrivateData = nullptr, DWORD cbPrivateData = 0);
    HRESULT CompleteConnect();
};

#endif // NDSESSION_HPP