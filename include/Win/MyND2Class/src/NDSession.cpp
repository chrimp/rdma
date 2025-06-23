#include "NDSession.hpp"
#include <cassert>
#include <iostream>
#include <algorithm>

NDSession::NDSession() :
    m_initialized(false),
    m_isServer(false),
    m_connected(false),
    m_assignedPort(0),
    m_adapter(nullptr),
    m_completionQueue(nullptr),
    m_queuePair(nullptr),
    m_connector(nullptr),
    m_listener(nullptr),
    m_defaultBufferSize(64 * 1024),
    m_poolSize(32),
    m_iocp(nullptr)
{
    ZeroMemory(&m_wsaData, sizeof(m_wsaData));
    ZeroMemory(&m_localAddr, sizeof(m_localAddr));
    ZeroMemory(&m_serverAddr, sizeof(m_serverAddr));
    ZeroMemory(&m_adapterInfo, sizeof(m_adapterInfo));

    InitializeCriticalSection(&m_lock);
}

NDSession::~NDSession() {
    Shutdown();
    DeleteCriticalSection(&m_lock);
}

HRESULT NDSession::Initialize(char* localAddr, ErrorCallback errorCb, DataCallback dataCb, ConnectionCallback connCb) {
    if (m_initialized) return S_OK;

    m_connectionCallback = connCb;
    m_errorCallback = errorCb;
    m_dataCallback = dataCb;

    HRESULT hr;

    hr = InitializeFramework();
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to initialize ND framework");
        return hr;
    }

    hr = SelectAndOpenAdapter(localAddr);
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to open ND adapter");
        return hr;
    }

    hr = QueryAdapter();
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to query and configure ND adapter");
        return hr;
    }

    hr = CreateAllResources();
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to create ND resources");
        return hr;
    }

    hr = CreateMemoryRegion();
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to setup memory pool");
        return hr;
    }

    m_initialized = true;
    return S_OK;
}

HRESULT NDSession::InitializeFramework() {
    int ret = WSAStartup(MAKEWORD(2, 2), &m_wsaData);
    if (ret != 0) {
        std::cerr << "WSAStartup failed with error: " << ret << std::endl;
        return HRESULT_FROM_WIN32(ret);
    }

    HRESULT hr = NdStartup();
    if (FAILED(hr)) {
        std::cerr << "NdStartup failed: " << std::hex << hr << std::endl;
        WSACleanup();
        return hr;
    }

    m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!m_iocp) {
        std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << std::endl;
        NdCleanup();
        WSACleanup();
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

HRESULT NDSession::SelectAndOpenAdapter(char* localAddr) {
    m_localAddr = { 0 };
    INT len = sizeof(m_localAddr);
    WSAStringToAddress(localAddr, AF_INET, nullptr, reinterpret_cast<struct sockaddr*>(&m_localAddr), &len);

    HRESULT hr = NdCheckAddress(reinterpret_cast<const struct sockaddr*>(&m_localAddr), sizeof(m_localAddr));
    if (FAILED(hr)) {
        std::cerr << "NdCheckAddress failed: " << std::hex << hr << std::endl;
        return hr;
    }
    
    hr = NdOpenAdapter(IID_IND2Adapter, reinterpret_cast<struct sockaddr*>(&m_localAddr), sizeof(m_localAddr), reinterpret_cast<void**>(&m_adapter));
    if (FAILED(hr)) {
        std::cerr << "NdOpenAdapter failed: " << std::hex << hr << std::endl;
        return hr;
    }
}

HRESULT NDSession::QueryAdapter() {
    m_adapterInfo.InfoVersion = ND_VERSION_2;
    ULONG adapterInfoSize = sizeof(m_adapterInfo);

    HRESULT hr = m_adapter->Query(&m_adapterInfo, &adapterInfoSize);
    if (FAILED(hr)) {
        std::cerr << "Query adapter info failed: " << std::hex << hr << std::endl;
        m_adapter->Release();
        m_adapter = nullptr;
        return hr;
    }
}

HRESULT NDSession::CreateAllResources() {
    HRESULT hr = CreateCompletionQueue();
    if (FAILED(hr)) return hr;

    hr = CreateQueuePair();
    if (FAILED(hr)) return hr;

    hr = CreateConnector();
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT NDSession::CreateCompletionQueue() {
    HRESULT hr = m_adapter->CreateOverlappedFile(&m_overlappedFile);

    ULONG depth = m_adapterInfo.MaxCompletionQueueDepth;
    hr = m_adapter->CreateCompletionQueue(IID_IND2CompletionQueue, m_overlappedFile, depth, 0, 0, reinterpret_cast<void**>(&m_completionQueue));
    if (FAILED(hr)) {
        std::cerr << "CreateCompletionQueue failed: " << std::hex << hr << std::endl;
        return hr;
    }
    
    return S_OK;
}

HRESULT NDSession::CreateQueuePair() {
    ULONG depth = m_adapterInfo.MaxCompletionQueueDepth;
    ULONG receiveSge = m_adapterInfo.MaxReceiveSge;
    ULONG readSge = m_adapterInfo.MaxReadSge;
    ULONG inlineDataSize = m_adapterInfo.MaxInlineDataSize;

    HRESULT hr = m_adapter->CreateQueuePair(IID_IND2QueuePair, m_completionQueue, m_completionQueue,
                                            nullptr, depth, depth, receiveSge, readSge, inlineDataSize,
                                            reinterpret_cast<void**>(&m_queuePair));
    if (FAILED(hr)) {
        std::cerr << "CreateQueuePair failed: " << std::hex << hr << std::endl;
        return hr;
    }
}

HRESULT NDSession::CreateConnector() {
    HRESULT hr = m_adapter->CreateConnector(IID_IND2Connector, m_overlappedFile, reinterpret_cast<void**>(&m_connector));
    if (FAILED(hr)) {
        std::cerr << "CreateConnector failed: " << std::hex << hr << std::endl;
        return hr;
    }
    
    return S_OK;
}

HRESULT NDSession::CreateMemoryRegion() {
    const size_t x_MaxXfer = 4 * 1024 * 1024;
    const size_t x_HdrLen = 40;

    HRESULT hr = m_adapter->CreateMemoryRegion(IID_IND2MemoryRegion, m_overlappedFile, reinterpret_cast<void**>(&m_memoryRegion));
    if (FAILED(hr)) {
        std::cerr << "CreateMemoryRegion failed: " << std::hex << hr << std::endl;
        return hr;
    }

    m_pBuf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, x_MaxXfer + x_HdrLen));
    if (!m_pBuf) {
        std::cerr << "HeapAlloc failed: " << GetLastError() << std::endl;
        m_memoryRegion->Release();
        m_memoryRegion = nullptr;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
    hr = m_memoryRegion->Register(m_pBuf, x_MaxXfer + x_HdrLen, flags, &m_ov);
    if (hr == ND_PENDING) {
        hr = m_memoryRegion->GetOverlappedResult(&m_ov, TRUE);
        if (SUCCEEDED(hr)) { hr = S_OK; }
    }

    if (FAILED(hr)) {
        std::cerr << "MemoryRegion Register failed: " << std::hex << hr << std::endl;
        HeapFree(GetProcessHeap(), 0, m_pBuf);
        m_memoryRegion->Release();
        m_memoryRegion = nullptr;
        return hr;
    }
}

HRESULT NDSession::CreateMemoryWindow() {
    HRESULT hr = m_adapter->CreateMemoryWindow(IID_IND2MemoryWindow, reinterpret_cast<void**>(&m_pMemoryWindow));
    return hr;
}

HRESULT NDSession::StartServer(USHORT port) {
    HRESULT hr = SetupServerListener(port);
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to setup server listener");
        return hr;
    }

    m_isServer = true;
    return S_OK;
}

HRESULT NDSession::ConnectToServer(const char* serverAddr, USHORT port) {
    HRESULT hr = EstablishClientConnection(serverAddr, port);
    if (FAILED(hr)) {
        if (m_errorCallback) m_errorCallback(hr, "Failed to connect to server");
        return hr;
    }

    m_connected = true;
    return S_OK;
}

HRESULT NDSession::Send(const void* data, size_t size, void* context) {
    if (!m_connected) return E_NOT_VALID_STATE;

    ManagedBuffer* buffer = GetAvailableBuffer(size);
    if (!buffer) return E_OUTOFMEMORY;

    memcpy(buffer->buffer, data, size);

    ND2_SGE sge = {0};
    sge.Buffer = buffer->buffer;
    sge.BufferLength = static_cast<ULONG>(size);
    sge.MemoryRegionToken = buffer->localToken;

    HRESULT hr = m_queuePair->Send(context, &sge, 1, 0);
    if (SUCCEEDED(hr)) buffer->inUse = true;
    
    return hr;
}

HRESULT NDSession::Receive(size_t expectedSize, void* context) {
    if (!m_connected) return E_NOT_VALID_STATE;

    ManagedBuffer* buffer = GetAvailableBuffer(expectedSize);
    if (!buffer) return E_OUTOFMEMORY;

    ND2_SGE sge = {0};
    sge.Buffer = buffer->buffer;
    sge.BufferLength = static_cast<ULONG>(expectedSize);
    sge.MemoryRegionToken = buffer->localToken;

    HRESULT hr = m_queuePair->Receive(context, &sge, 1);
    if (SUCCEEDED(hr)) buffer->inUse = true;
    return hr;
}

HRESULT NDSession::EstablishClientConnection(const char* serverAddr, USHORT port) {
    sockaddr_in serverAddress = {0};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    inet_pton(AF_INET, serverAddr, &serverAddress.sin_addr);

    HRESULT hr  = m_connector->Bind(reinterpret_cast<const sockaddr*>(&m_localAddr), sizeof(m_localAddr));
    if (FAILED(hr)) {
        std::cerr << "Bind failed: " << std::hex << hr << std::endl;
        return hr;
    }

    OVERLAPPED connectOv = {0};
    connectOv.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    hr = m_connector->Connect(m_queuePair, reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress), 0, 0, nullptr, 0, &connectOv);

    if (hr == ND_PENDING) {
        WaitForSingleObject(connectOv.hEvent, INFINITE);
        hr = m_connector->GetOverlappedResult(&connectOv, TRUE);
    }

    CloseHandle(connectOv.hEvent);
    return hr;
}

HRESULT NDSession::SetupServerListener(USHORT port) {
    // TODO: Create and setup listener
    HRESULT hr = m_adapter->CreateListener(IID_IND2Listener, m_overlappedFile,
                                          reinterpret_cast<void**>(&m_listener));
    if (FAILED(hr)) return hr;
    
    // Bind to local address
    sockaddr_in bindAddr = m_localAddr;
    bindAddr.sin_port = htons(port);
    
    hr = m_listener->Bind(reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr));
    if (FAILED(hr)) return hr;
    
    // Start listening
    hr = m_listener->Listen(1); // backlog of 1
    if (FAILED(hr)) return hr;
    
    // Get assigned port if port was 0
    ULONG addrSize = sizeof(bindAddr);
    hr = m_listener->GetLocalAddress(reinterpret_cast<sockaddr*>(&bindAddr), &addrSize);
    if (SUCCEEDED(hr)) {
        m_assignedPort = ntohs(bindAddr.sin_port);
    }
    
    return hr;
}

HRESULT NDSession::ProcessEvents(DWORD timeoutms) {
    // TODO: Process completion queue events
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    OVERLAPPED* pOverlapped;
    
    BOOL result = GetQueuedCompletionStatus(m_iocp, &bytesTransferred, 
                                           &completionKey, &pOverlapped, timeoutms);
    
    if (result || pOverlapped) {
        // Handle completion
        return HandleCompletion(bytesTransferred, completionKey, pOverlapped);
    }
    
    return HRESULT_FROM_WIN32(GetLastError());
}

HRESULT NDSession::HandleCompletion(const ND2_RESULT& result) {
    // TODO: Handle different completion types
    switch (result.RequestType) {
        case Nd2RequestTypeSend:
            // Handle send completion
            if (m_dataCallback) {
                m_dataCallback(nullptr, result.BytesTransferred, result.RequestContext);
            }
            break;
            
        case Nd2RequestTypeReceive:
            // Handle receive completion
            if (m_dataCallback) {
                // Find buffer from context and call callback
                m_dataCallback(/* buffer */, result.BytesTransferred, result.RequestContext);
            }
            break;
            
        // Handle other request types...
    }
    
    return S_OK;
}

NDSession::ManagedBuffer* NDSession::GetAvailableBuffer(size_t size) {
    EnterCriticalSection(&m_lock);
    
    // Find available buffer of sufficient size
    for (auto& buffer : m_memoryPool) {
        if (!buffer.inUse && buffer.size >= size) {
            buffer.inUse = true;
            LeaveCriticalSection(&m_lock);
            return &buffer;
        }
    }
    
    // Allocate new buffer if none available
    ManagedBuffer newBuffer = {0};
    size_t allocSize = std::max(size, m_defaultBufferSize);
    
    HRESULT hr = AllocateAndReigsterBuffer(allocSize, 
                                          ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE,
                                          &newBuffer.buffer, &newBuffer.localToken);
    
    if (SUCCEEDED(hr)) {
        newBuffer.size = allocSize;
        newBuffer.inUse = true;
        m_memoryPool.push_back(newBuffer);
        LeaveCriticalSection(&m_lock);
        return &m_memoryPool.back();
    }
    
    LeaveCriticalSection(&m_lock);
    return nullptr;
}