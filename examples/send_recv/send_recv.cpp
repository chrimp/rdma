#include "NDSession.hpp"
#include <iostream>

constexpr int TEST_BUFFER_SIZE = 10240;
constexpr char TEST_PORT[] = "54321";

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

struct PeerInfo {
    UINT64 remoteAddr;
    UINT32 remoteToken;
};


void ShowUsage() {
    printf("rdma.exe [options]\n"
           "Options:\n"
           "\t-s <local_ip>           - Start as server\n"
           "\t-c <local_ip> <server_ip> - Start as client\n");
}

// MARK: TestServer
class TestServer : public NDSessionServerBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        std::cout << "Max transfer length: " << info.MaxTransferLength << std::endl;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(TEST_BUFFER_SIZE, flags))) return false;

        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = TEST_BUFFER_SIZE;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        PostReceive(&sge, 1, RECV_CTXT);
        //PostReceive(&sge, 1, RECV_CTXT);

        if (FAILED(CreateListener())) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    void Run(const char* localAddr) {
        char fullAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullAddress, "%s:%s", localAddr, TEST_PORT);
        std::cout << "Listening on " << fullAddress << "..." << std::endl;
        if (FAILED(Listen(fullAddress))) return;

        std::cout << "Waiting for connection request..." << std::endl;
        if (FAILED(GetConnectionRequest())) {
            std::cout << "GetConnectionRequest failed. Reason: " << std::hex << GetResult() << std::endl;
            return;
        }

        std::cout << "Accepting connection..." << std::endl;
        if (FAILED(Accept(1, 1, nullptr, 0))) return;
        
        std::cout << "Connection established. Waiting for message from client..." << std::endl;

        WaitForCompletionAndCheckContext(RECV_CTXT);
        std::cout << "Received from client: '" << (char*)m_Buf << "'" << std::endl;

        CreateMW();
        Bind(m_Buf, TEST_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE);
        
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = TEST_BUFFER_SIZE;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        // Send a response
        const char* response = "Message received by server.";
        strcpy_s((char*)m_Buf, TEST_BUFFER_SIZE, response);
        sge.BufferLength = (ULONG)strlen(response) + 1;

        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for send failed." << std::endl;
            return;
        }

        ND2_RESULT sendRes = WaitForCompletion(false);
        if (sendRes.Status != ND_PENDING) abort();

        std::cout << "Response sent. Waiting for client's PeerInfo..." << std::endl;

        /*
        InvalidateMW();
        WaitForCompletion(true);
        HRESULT hr = CreateMW();
        if (FAILED(hr)) abort();
        hr = RegisterDataBuffer(sizeof(PeerInfo), ND_MR_FLAG_ALLOW_REMOTE_READ | ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE);
        if (FAILED(hr)) abort();
        

        std::variant<HRESULT, ND2_RESULT> bindRes = Bind(m_Buf, sizeof(PeerInfo), ND_OP_FLAG_ALLOW_READ | ND_OP_FLAG_ALLOW_WRITE);
        if (std::holds_alternative<HRESULT>(bindRes)) {
            HRESULT bindHr = std::get<HRESULT>(bindRes);
            if (FAILED(bindHr)) abort();
        } else {
            ND2_RESULT ndRes = std::get<ND2_RESULT>(bindRes);
            if (ndRes.Status != ND_SUCCESS) abort();
        }
        */

        sge = { m_Buf, TEST_BUFFER_SIZE, m_pMr->GetLocalToken() };
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for PeerInfo failed." << std::endl;
            return;
        }
        
        PeerInfo* remoteInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        std::cout << "Received PeerInfo from client: remoteAddr = " << remoteInfo->remoteAddr
                  << ", remoteToken = " << remoteInfo->remoteToken << std::endl;


        /*
        PeerInfo holder = *remoteInfo;

        InvalidateMW();
        WaitForCompletion(true);
        CreateMW();
        hr = RegisterDataBuffer(sizeof(PeerInfo), ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE | ND_MR_FLAG_ALLOW_REMOTE_READ);

        reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr = holder.remoteAddr;
        reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken = holder.remoteToken;
        */

        /*
        InvalidateMW();
        WaitForCompletion(true);
        CreateMW();
        hr = RegisterDataBuffer(sizeof(TEST_BUFFER_SIZE), ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE | ND_MR_FLAG_ALLOW_REMOTE_READ);
        Bind(m_Buf, sizeof(TEST_BUFFER_SIZE), ND_OP_FLAG_ALLOW_READ | ND_OP_FLAG_ALLOW_WRITE);
        

        const char* msg = "line 151";
        size_t len = strlen(msg) + 1;
        std::memcpy(m_Buf, msg, len);
        */

        sge = { m_Buf, sizeof(TEST_BUFFER_SIZE), m_pMr->GetLocalToken() };

        //Sleep(1000);

        // Echo back the PeerInfo
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send of PeerInfo failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for PeerInfo send failed." << std::endl;
            return;
        }

        std::cout << "Echoed PeerInfo back to client. Sending my PeerInfo..." << std::endl;

        ZeroMemory(m_Buf, TEST_BUFFER_SIZE);
        
        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMr->GetLocalToken();

        std::cout << "m_Buf size is " << HeapSize(GetProcessHeap(), 0, m_Buf) << std::endl;

        sge.BufferLength = TEST_BUFFER_SIZE;
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send of my PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for my PeerInfo send failed." << std::endl;
            return;
        }

        std::cout << "My PeerInfo sent to client. Waiting for echo..." << std::endl;

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for echoed PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for echoed PeerInfo receive failed." << std::endl;
            return;
        }

        // Compare the received PeerInfo
        bool conditionOne = (myInfo->remoteAddr == reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr);
        bool conditionTwo = (myInfo->remoteToken == reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken);

        if (!(conditionOne && conditionTwo)) {
            std::cerr << "Received PeerInfo does not match sent PeerInfo." << std::endl;
        } else {
            std::cout << "Received echoed PeerInfo matches sent PeerInfo." << std::endl;
        }

        Shutdown();
    }
};

// MARK: TestClient
class TestClient : public NDSessionClientBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(TEST_BUFFER_SIZE, flags))) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    void Run(const char* localAddr, const char* serverAddr) {
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = TEST_BUFFER_SIZE;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive failed." << std::endl;
            return;
        }

        char fullServerAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullServerAddress, "%s:%s", serverAddr, TEST_PORT);

        std::cout << "Connecting from " << localAddr << " to " << fullServerAddress << "..." << std::endl;
        if (FAILED(Connect(localAddr, fullServerAddress, 1, 1, nullptr, 0))) {
             std::cerr << "Connect failed." << std::endl;
             return;
        }

        if (FAILED(CompleteConnect())) {
            std::cerr << "CompleteConnect failed." << std::endl;
            return;
        }
        std::cout << "Connection established." << std::endl;

        CreateMW();
        Bind(m_Buf, TEST_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE);

        // Send a message
        const char* message = "Hello from client.";
        strcpy_s((char*)m_Buf, TEST_BUFFER_SIZE, message);
        sge = { m_Buf, TEST_BUFFER_SIZE, m_pMr->GetLocalToken() };

        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for send failed." << std::endl;
            return;
        }
        std::cout << "Message sent. Waiting for response..." << std::endl;

        // Post a receive for the server's response
        sge.BufferLength = TEST_BUFFER_SIZE;
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for receive failed." << std::endl;
            return;
        }

        std::cout << "Received from server: '" << (char*)m_Buf << "'" << std::endl;

        ND2_RESULT res = WaitForCompletion(false);
        if (res.Status == ND_PENDING) {
            std::cout << "There's no more pending operations." << std::endl;
        }

        /*
        When resizing the buffer:
        - Invalidate the memory window
        - Wait for completion of the invalidate operation
        - Deregister the old memory region
        - Create a new memory window
        - Register the new buffer
        - Bind the new buffer
        */
        
        /*
        if (FAILED(InvalidateMW())) abort();
        WaitForCompletion(true);
        CreateMW();
        HRESULT hr = RegisterDataBuffer(sizeof(PeerInfo), ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE);
        if (FAILED(hr)) abort();
        std::variant<HRESULT, ND2_RESULT> bindRes = Bind(m_Buf, sizeof(PeerInfo), ND_OP_FLAG_ALLOW_WRITE);
        if (std::holds_alternative<HRESULT>(bindRes)) {
            HRESULT bindHr = std::get<HRESULT>(bindRes);
            if (FAILED(bindHr)) abort();
        } else {
            ND2_RESULT ndRes = std::get<ND2_RESULT>(bindRes);
            if (ndRes.Status != ND_SUCCESS) abort();
        }
        */

        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMr->GetLocalToken();

        PeerInfo myInfoHolder;
        myInfoHolder.remoteAddr = myInfo->remoteAddr;
        myInfoHolder.remoteToken = myInfo->remoteToken;

        std::cout << "My address: " << myInfo->remoteAddr << ", token: " << myInfo->remoteToken << std::endl;

        sge = { m_Buf, TEST_BUFFER_SIZE, m_pMr->GetLocalToken() };
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for send failed." << std::endl;
            return;
        }

        std::cout << "Peer information sent. Waiting for echo..." << std::endl;

        /*
        //InvalidateMW();
        //WaitForCompletion(true);
        //CreateMW();
        RegisterDataBuffer(sizeof(TEST_BUFFER_SIZE), ND_MR_FLAG_ALLOW_REMOTE_WRITE | ND_MR_FLAG_ALLOW_LOCAL_WRITE);
        //Bind(m_Buf, sizeof(TEST_BUFFER_SIZE), ND_OP_FLAG_ALLOW_WRITE);
        */

        sge = { m_Buf, sizeof(TEST_BUFFER_SIZE), m_pMr->GetLocalToken() };
        // What is this error, why use sizeof(TEST_BUFFER_SIZE) here?
        // But why the second receive overruns?

        CheckForOPs();

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for PeerInfo failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for PeerInfo receive failed." << std::endl;
            return;
        }

        // Compare the received PeerInfo
        bool conditionOne = (myInfo->remoteAddr == reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr);
        bool conditionTwo = (myInfo->remoteToken == reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken);
        if (!(conditionOne && conditionTwo)) {
            std::cerr << "Received PeerInfo does not match sent PeerInfo." << std::endl;
        } else {
            std::cout << "Received echoed PeerInfo matches sent PeerInfo." << std::endl;
            std::cout << "Remote address: " << reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr
                        << ", Remote token: " << reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken << std::endl;
            std::cout << "My address: " << myInfoHolder.remoteAddr <<
                        ", My token: " << myInfoHolder.remoteToken << std::endl;
        }

        std::cout << "Waiting for server's PeerInfo..." << std::endl;

        sge.BufferLength = TEST_BUFFER_SIZE;

        std::cout << "m_Buf size is " << HeapSize(GetProcessHeap(), 0, m_Buf) << std::endl;

        ZeroMemory(m_Buf, TEST_BUFFER_SIZE);

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for server's PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) { // Overrun?
            std::cerr << "WaitForCompletion for server's PeerInfo failed." << std::endl;
            return;
        }

        PeerInfo* serverInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        std::cout << "Received server's PeerInfo: remoteAddr = " << serverInfo->remoteAddr
                    << ", remoteToken = " << serverInfo->remoteToken << std::endl;
        
        // Echo back the PeerInfo
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send of PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for PeerInfo send failed." << std::endl;
            return;
        }

        std::cout << "Echoed PeerInfo back to server. Shutting down." << std::endl;

        Shutdown();
    }
};

void PrintAvailableRdmaAddresses() {
    SIZE_T bufferSize = 0;
    HRESULT hr = NdQueryAddressList(0, nullptr, &bufferSize);
    if (hr != ND_BUFFER_OVERFLOW) {
        std::cerr << "Could not query for RDMA addresses. Error: " << std::hex << hr << std::endl;
        return;
    }

    if (bufferSize == 0) {
        std::cout << "No RDMA-capable adapters found on this system." << std::endl;
        return;
    }

    SOCKET_ADDRESS_LIST* pAddressList = (SOCKET_ADDRESS_LIST*)new char[bufferSize];
    if (!pAddressList) {
        std::cerr << "Failed to allocate memory for address list." << std::endl;
        return;
    }

    hr = NdQueryAddressList(0, pAddressList, &bufferSize);
    if (FAILED(hr)) {
        std::cerr << "NdQueryAddressList failed with error: " << std::hex << hr << std::endl;
        delete[] pAddressList;
        return;
    }

    std::cout << "Available RDMA-capable IP addresses:" << std::endl;
    for (int i = 0; i < pAddressList->iAddressCount; ++i) {
        char ipStr[INET6_ADDRSTRLEN];
        DWORD ipStrLen = INET6_ADDRSTRLEN;
        if (WSAAddressToString(
            pAddressList->Address[i].lpSockaddr,
            pAddressList->Address[i].iSockaddrLength,
            nullptr,
            ipStr,
            &ipStrLen) == 0) {
            std::cout << "  - " << ipStr << std::endl;
        }
    }

    delete[] pAddressList;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        ShowUsage();
        return 1;
    }

    bool isServer = false;
    if (strcmp(argv[1], "-s") == 0) {
        if (argc != 3) { ShowUsage(); return 1; }
        isServer = true;
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc != 4) { ShowUsage(); return 1; }
        isServer = false;
    } else {
        ShowUsage();
        return 1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    if (FAILED(NdStartup())) {
        std::cerr << "NdStartup failed." << std::endl;
        WSACleanup();
        return 1;
    }

    if (isServer) {
        TestServer server;
        if (server.Setup(argv[2])) {
            server.Run(argv[2]);
        } else {
            std::cerr << "Server setup failed." << std::endl;
        }
    } else { // Client
        TestClient client;
        // Use the specific local IP for setup, and pass both to Run
        if (client.Setup(argv[2])) {
            client.Run(argv[2], argv[3]);
        } else {
            std::cerr << "Client setup failed." << std::endl;
        }
    }

    NdCleanup();
    WSACleanup();
    return 0;
}