#include "NDSession.hpp"
#include <iostream>

/*
* ======================================================================
* NOTICE: This code contains substantial AI-generated content based on
* human-provided prompts, specifications, or design inputs.
*
* The code is provided as-is without warranty or guarantee of accuracy.
* AI-generated portions have not been thoroughly reviewed and may
* contain errors, logical flaws, or incomplete implementations.
*
* You are responsible for thorough review, testing, and validation
* before use. Exercise caution and comprehensive testing practices,
* as AI-generated code may introduce subtle issues not immediately
* apparent.
* ======================================================================
*/

constexpr int TEST_BUFFER_SIZE = 1024;
constexpr char TEST_PORT[] = "54321";

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)


void ShowUsage() {
    printf("rdma.exe [options]\n"
           "Options:\n"
           "\t-s <local_ip>           - Start as server\n"
           "\t-c <local_ip> <server_ip> - Start as client\n");
}

// A simple server implementation using your base class
class TestServer : public NDSessionServerBase {
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

        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = TEST_BUFFER_SIZE;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        PostReceive(&sge, 1, RECV_CTXT);
        PostReceive(&sge, 1, RECV_CTXT);

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

        std::cout << "Response sent. Shutting down." << std::endl;
        Shutdown();
    }
};

// A simple client implementation
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
        sge = { m_Buf, (ULONG)strlen(message) + 1, m_pMr->GetLocalToken() };

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
        if (WSAAddressToStringA(
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