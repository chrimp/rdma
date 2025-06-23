#ifndef NDSESSION_HPP
#define NDSESSION_HPP
#pragma once

#include <functional>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <vector>
#include <memory>
#include <string>
#include <ndsupport.h>

class NDSession {
    public:
    using ErrorCallback = std::function<void(HRESULT hr, const char* context)>;
    using DataCallback = std::function<void(void* buffer, size_t size, void* context)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    NDSession();
    ~NDSession();

    HRESULT Initialize(char* localAddr, ErrorCallback errorCb = nullptr, 
                       DataCallback dataCb = nullptr, ConnectionCallback connCb = nullptr);

    HRESULT StartServer(USHORT port = 0);
    HRESULT ConnectToServer(const char* serverAddr, USHORT port);

    HRESULT Send(const void* data, size_t size, void* context = nullptr);
    HRESULT Receive(size_t expectedSize, void* context = nullptr);
    HRESULT Read(UINT64 remoteAddr, UINT32 remoteKey, size_t size, void* context = nullptr);
    HRESULT Write(const void* data, size_t size, UINT64 remoteAddr, UINT32 remoteKey, void* context = nullptr);

    HRESULT GetMemoryInfo(void* buffer, UINT64* remoteAddr, UINT32* remoteKey);

    HRESULT ProcessEvents(DWORD timeoutms = 0);

    USHORT GetListenPort() const { return m_assignedPort; }

    void Shutdown();

    private:
    HRESULT InitializeFramework();
    HRESULT SelectAndOpenAdapter(char* localAddr);
    HRESULT QueryAdapter();
    HRESULT CreateAllResources();

    HRESULT CreateCompletionQueue();
    HRESULT CreateQueuePair();
    HRESULT CreateConnector();
    HRESULT CreateMemoryRegion();
    HRESULT CreateMemoryWindow();

    HRESULT SetupServerListener(USHORT port);
    HRESULT EstablishClientConnection(const char* serverAddr, USHORT port);
    HRESULT HandleConnectionEvents();

    HRESULT AllocateAndReigsterBuffer(size_t size, ULONG flags, void** buffer, UINT32* token);
    HRESULT FindMemoryRegistration(void* buffer, UINT32* localToken, UINT32* remoteToken, UINT64* remoteAddr);
    void CleanupAllMemory();

    HRESULT ProcessCompletions();
    HRESULT HandleCompletion(const ND2_RESULT& result);

    bool m_initialized;
    bool m_isServer;
    bool m_connected;
    USHORT m_assignedPort;

    ConnectionCallback m_connectionCallback;
    ErrorCallback m_errorCallback;
    DataCallback m_dataCallback;

    WSADATA m_wsaData;
    sockaddr_in m_localAddr;
    sockaddr_in m_serverAddr;

    IND2Adapter* m_adapter;
    ND2_ADAPTER_INFO m_adapterInfo;
    IND2CompletionQueue* m_completionQueue;
    IND2QueuePair* m_queuePair;
    IND2Connector* m_connector;
    IND2Listener* m_listener;

    IND2MemoryRegion* m_memoryRegion;
    IND2MemoryWindow* m_pMemoryWindow;
    void* m_pBuf;

    OVERLAPPED m_ov;
    
    struct ManagedBuffer {
        void* buffer;
        size_t size;
        IND2MemoryRegion* memoryRegion;
        UINT32 localToken;
        UINT32 remoteToken;
        bool inUse;
    };

    NDSession::ManagedBuffer* GetAvailableBuffer(size_t size);

    std::vector<ManagedBuffer> m_memoryPool;
    size_t m_defaultBufferSize;
    size_t m_poolSize;

    struct InternalConfig {
        ULONG queueDepth;
        ULONG maxSge;
        ULONG inlineThreshold;
        ULONG maxTransferSize;
        ULONG maxReads;
    } m_config;

    struct PendingOp {
        OVERLAPPED overlapped;
        enum Type { CONNECT, ACCEPT, SEND, RECV, READ, WRITE } type;
        void* userContext;
        ManagedBuffer* buffer;
    };

    std::vector<std::unique_ptr<PendingOp>> m_pendingOps;

    HANDLE m_iocp;

    HANDLE m_overlappedFile;

    CRITICAL_SECTION m_lock;
    
};

#endif // NDSESSION_HPP