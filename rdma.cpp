// rdma.cpp : Defines the entry point for the application.
//
#include <iostream>
#include <ndsupport.h>
#include <WS2tcpip.h>
#include <vector>
#include <memory>

const size_t x_MaxXfer = 4 * 1024 * 1024;
const size_t x_HdrLen = 40;

#define RECV_CTXT ((void*) 0x1000)
#define SEND_CTXT ((void*) 0x2000)
#define READ_CTXT ((void*) 0x3000)
#define WRITE_CTXT((void*) 0x4000)

class RDMAConnection {
    private:
    IND2Adapter* adapter;
    IND2CompletionQueue* cq;
    IND2QueuePair* qp;
    IND2MemoryRegion* mr;
    IND2Connector* connector;
    HANDLE hOverlappedFile;
    void* m_pBuf;

    OVERLAPPED m_ov;

    public:
    RDMAConnection() : adapter(nullptr), cq(nullptr), qp(nullptr), mr(nullptr) {}

    ~RDMAConnection() { Cleanup(); }

    HRESULT Initialize(char* localAddress) {
        HRESULT hr;

        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        sockaddr_in v4 = { 0 };
        INT len = sizeof(v4);
        WSAStringToAddress(localAddress, AF_INET, nullptr, reinterpret_cast<struct sockaddr*>(&v4), &len);

        hr = NdStartup();
        if (FAILED(hr)) {
            std::cerr << "NdStartup failed: " << std::hex << hr << std::endl;
            return hr;
        }

        hr = NdCheckAddress(reinterpret_cast<const struct sockaddr*>(&v4), sizeof(v4));
        if (FAILED(hr)) {
            std::cerr << "NdCheckAddress failed: " << std::hex << hr << std::endl;
            return hr;
        }

        hr = NdOpenAdapter(IID_IND2Adapter, reinterpret_cast<struct sockaddr*>(&v4), sizeof(v4), reinterpret_cast<void**>(&adapter));
        if (FAILED(hr)) {
            std::cerr << "NdOpenAdapter failed: " << std::hex << hr << std::endl;
            return hr;
        }

        ND2_ADAPTER_INFO adapterInfo = { 0 };
        adapterInfo.InfoVersion = ND_VERSION_2;
        ULONG adapterInfoSize = sizeof(adapterInfo);

        hr = adapter->Query(&adapterInfo, &adapterInfoSize);
        if (FAILED(hr)) {
            std::cerr << "Query adapter info failed: " << std::hex << hr << std::endl;
            return hr;
        }

        ULONG depth = adapterInfo.MaxCompletionQueueDepth;
        hr = adapter->CreateOverlappedFile(&hOverlappedFile);
        if (FAILED(hr)) {
            std::cerr << "CreateOverlappedFile failed: " << std::hex << hr << std::endl;
            return hr;
        }

        hr = adapter->CreateCompletionQueue(IID_IND2QueuePair, hOverlappedFile, depth, 0, 0, reinterpret_cast<void**>(&cq));
        if (FAILED(hr)) {
            std::cerr << "CreateCompletionQueue failed: " << std::hex << hr << std::endl;
            return hr;
        }

        ULONG receiveSge  = adapterInfo.MaxReceiveSge;
        ULONG readSge = adapterInfo.MaxReadSge;
        ULONG inlineDataSize = adapterInfo.MaxInlineDataSize;

        hr = adapter->CreateQueuePair(IID_IND2QueuePair, cq, cq, nullptr, depth, depth, receiveSge, readSge, inlineDataSize, reinterpret_cast<void**>(&qp));
        if (FAILED(hr)) {
            std::cerr << "CreateQueuePair failed: " << std::hex << hr << std::endl;
            return hr;
        }

        hr = adapter->CreateConnector(IID_IND2Connector, hOverlappedFile, reinterpret_cast<void**>(&connector));
        if (FAILED(hr)) {
            std::cerr << "CreateConnector failed: " << std::hex << hr << std::endl;
            return hr;
        }

        hr = adapter->CreateMemoryRegion(IID_IND2MemoryRegion, hOverlappedFile, reinterpret_cast<void**>(&mr));
        if (FAILED(hr)) {
            std::cerr << "CreateMemoryRegion failed: " << std::hex << hr << std::endl;
            return hr;
        }

        m_pBuf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, x_MaxXfer + x_HdrLen));
        if (!m_pBuf) {
            std::cerr << "HeapAlloc failed" << std::endl;
            return E_OUTOFMEMORY;
        }

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;

        hr = mr->Register(m_pBuf, x_MaxXfer + x_HdrLen, flags, &m_ov);
        if (FAILED(hr)) {
            std::cerr << "MemoryRegion Register failed: " << std::hex << hr << std::endl;
            HeapFree(GetProcessHeap(), 0, m_pBuf);
            return hr;
        }

        ND2_SGE sge = { 0 };
        sge.Buffer = m_pBuf;
        sge.BufferLength = x_MaxXfer + x_HdrLen;
        sge.MemoryRegionToken = mr->GetLocalToken();
        hr = qp->Receive(RECV_CTXT, &sge, 1);

        return S_OK;
    }

    HRESULT Connect(char* remoteAddress, USHORT port) {
        sockaddr_in v4 = { 0 };
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        inet_pton(AF_INET, remoteAddress, &v4.sin_addr);

        OVERLAPPED overlapped = { 0 };
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        overlapped.hEvent = event;
    }
};




int main(int arcg, char* argv[])
{
    bool bServer = false;
    bool bClient = false;
    struct sockaddr_in v4Server = {0 };
    DWORD nSge = 1;
    
    WSADATA wsaData;

	return 0;
}
