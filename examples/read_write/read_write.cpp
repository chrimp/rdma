#include "NDSession.hpp"
#include <iostream>
#include <chrono>
#include <cstring>

constexpr int TEST_BUFFER_SIZE = 1024 * 1024; // 1MB
constexpr int NUM_ITER = 50000;
constexpr char TEST_PORT[] = "54321";

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

struct PeerInfo {
    UINT64 remoteAddr;
    UINT32 remoteToken;
};

class RWTestServer : public NDSessionServerBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;
        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;
        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE | ND_MR_FLAG_ALLOW_REMOTE_READ;
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
        if (FAILED(GetConnectionRequest())) return;
        if (FAILED(Accept(1, 1, nullptr, 0))) return;
        std::cout << "Connection established.\n";

        WaitForCompletionAndCheckContext(RECV_CTXT);

        CreateMW();
        Bind(m_Buf, TEST_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE);

        // Receive PeerInfo from client
        PeerInfo peerInfo;
        ND2_SGE recvSge = { &peerInfo, sizeof(PeerInfo), m_pMr->GetLocalToken() };
        if (FAILED(PostReceive(&recvSge, 1, RECV_CTXT))) return;
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) return;

        // Throughput test: Write NUM_ITER times to the client's buffer
        std::cout << "Starting throughput test (" << NUM_ITER << " ops, " << (TEST_BUFFER_SIZE/1024) << " KB each)...\n";
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_ITER; ++i) {
            memset(m_Buf, i & 0xFF, TEST_BUFFER_SIZE);
            ND2_SGE wrSge = { m_Buf, TEST_BUFFER_SIZE, m_pMr->GetLocalToken() };
            if (FAILED(Write(&wrSge, 1, peerInfo.remoteAddr, peerInfo.remoteToken, 0, WRITE_CTXT))) return;
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) return;
        }
        auto end = std::chrono::high_resolution_clock::now();
        double seconds = std::chrono::duration<double>(end - start).count();
        double mb = (double)NUM_ITER * TEST_BUFFER_SIZE / (1024.0 * 1024.0);
        std::cout << "Throughput: " << mb / seconds << " MB/s\n";

        // Latency test: Write 1 byte NUM_ITER times to the client's buffer
        std::cout << "Starting latency test (" << NUM_ITER << " single-byte writes)...\n";
        char one = 42;
        ND2_SGE oneSge = { &one, 1, m_pMr->GetLocalToken() };
        start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_ITER; ++i) {
            if (FAILED(Write(&oneSge, 1, peerInfo.remoteAddr, peerInfo.remoteToken, 0, WRITE_CTXT))) return;
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) return;
        }
        end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count() / NUM_ITER;
        std::cout << "Average latency: " << us << " us/op\n";

        Shutdown();
    }
};

class RWTestClient : public NDSessionClientBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;
        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;
        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;
        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE | ND_MR_FLAG_ALLOW_REMOTE_READ;
        if (FAILED(RegisterDataBuffer(TEST_BUFFER_SIZE, flags))) return false;
        if (FAILED(CreateConnector())) return false;
        return true;
    }

    void Run(const char* localAddr, const char* serverAddr) {
        char fullServerAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullServerAddress, "%s:%s", serverAddr, TEST_PORT);
        if (FAILED(Connect(localAddr, fullServerAddress, 1, 1, nullptr, 0))) return;
        if (FAILED(CompleteConnect())) return;
        std::cout << "Connected to server.\n";

        CreateMW();
        Bind(m_Buf, TEST_BUFFER_SIZE, ND_OP_FLAG_ALLOW_READ | ND_OP_FLAG_ALLOW_WRITE);

        // Send PeerInfo to server
        PeerInfo myInfo;
        myInfo.remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo.remoteToken = m_pMr->GetLocalToken();
        ND2_SGE sge = { &myInfo, sizeof(PeerInfo), m_pMr->GetLocalToken() };
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) return;
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) return;

        // Wait for server's RDMA ops (validate buffer content)
        std::cout << "Ready for server's RDMA operations.\n";
        volatile char* buf = static_cast<volatile char*>(m_Buf);
        for (int i = 0; i < NUM_ITER; ++i) {
            char expected = static_cast<char>(i & 0xFF);
            while (buf[0] != expected) {
                // Optionally sleep to reduce CPU usage
                // std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        Shutdown();
    }
};

void ShowUsage() {
    printf("rdma_rw_minimal.exe [options]\n"
           "Options:\n"
           "\t-s <local_ip>           - Start as server\n"
           "\t-c <local_ip> <server_ip> - Start as client\n");
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
        RWTestServer server;
        if (server.Setup(argv[2])) {
            server.Run(argv[2]);
        } else {
            std::cerr << "Server setup failed." << std::endl;
        }
    } else { // Client
        RWTestClient client;
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