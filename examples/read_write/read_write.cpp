#include "NDSession.hpp"
#include <iostream>
#include <thread>
#include <chrono>

constexpr char TEST_PORT[] = "54321";

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

#undef max
#undef min

constexpr size_t CHUNK_SIZE = 536870912ULL;
constexpr size_t TEST_BUFFER_SIZE = std::max(536870912ULL, CHUNK_SIZE); // At least 512MB or chunk size

static size_t maxSge = 32; // Temporary value for deciding test size
static size_t THROUGHPUT_TEST_SIZE = CHUNK_SIZE * maxSge;
static int NUM_CHUNKS = static_cast<int>(THROUGHPUT_TEST_SIZE / CHUNK_SIZE);  // 20 chunks

constexpr size_t RTT_TEST_SIZE = 1;  // Small size for RTT test
constexpr int RTT_TEST_ITERATIONS = 10;  // Number of ping-pong iterations

std::string FormatBytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        return std::to_string(bytes / (1024ULL * 1024 * 1024)) + "GB";
    } else if (bytes >= 1024ULL * 1024) {
        return std::to_string(bytes / (1024ULL * 1024)) + "MB";
    } else if (bytes >= 1024ULL) {
        return std::to_string(bytes / 1024ULL) + "KB";
    } else {
        return std::to_string(bytes) + "B";
    }
}

uint64_t GetCurrentTimestamp() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

double CalculateGbps(uint64_t bytes, uint64_t nanoseconds) {
    return (static_cast<double>(bytes) * 8.0) / (static_cast<double>(nanoseconds) / 1e9) / 1e9;
}

double CalculateLatencyMicroseconds(uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1000.0;
}

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
        std::cout << "Max inline length: " << info.MaxInlineDataSize << std::endl;
        std::cout << "Max send sge: " << info.MaxInitiatorSge << std::endl;

        maxSge = info.MaxInitiatorSge;
        THROUGHPUT_TEST_SIZE = CHUNK_SIZE * maxSge;
        NUM_CHUNKS = static_cast<int>(THROUGHPUT_TEST_SIZE / CHUNK_SIZE);

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(TEST_BUFFER_SIZE, flags))) return false;

        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = TEST_BUFFER_SIZE;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        // Do not bind here, it leads to QP failure

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
        
        CreateMW();
        Bind(m_Buf, TEST_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

        std::cout << "Connection established. Waiting for client's PeerInfo..." << std::endl;
        std::cout << "My address: " << reinterpret_cast<UINT64>(m_Buf) << ", token: " << m_pMw->GetRemoteToken() << std::endl;
        
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = sizeof(PeerInfo);
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for PeerInfo failed." << std::endl;
            return;
        }
        
        PeerInfo* receivedInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        std::cout << "Received PeerInfo from client: remoteAddr = " << receivedInfo->remoteAddr
                  << ", remoteToken = " << receivedInfo->remoteToken << std::endl;

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };

        PeerInfo remoteInfo;
        remoteInfo.remoteAddr = receivedInfo->remoteAddr;
        remoteInfo.remoteToken = receivedInfo->remoteToken;

        ZeroMemory(m_Buf, TEST_BUFFER_SIZE);
        
        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMw->GetRemoteToken();

        sge.BufferLength = sizeof(PeerInfo);

        std::this_thread::sleep_for(std::chrono::seconds(1)); // Give time for client to prepare

        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send of my PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for my PeerInfo send failed." << std::endl;
            return;
        }

        // Get client's notification
        if (FAILED(PostReceive(nullptr, 0, RECV_CTXT))) {
            std::cerr << "PostReceive for notification failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for notification failed." << std::endl;
            return;
        }

        std::cout << "TEST: Wrtie throughput test (Server -> Client)" << std::endl;
        std::cout << "Writing " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE)
                  << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;

        size_t totalWritten = 0;
        auto startTime = std::chrono::high_resolution_clock::now();

        memset(m_Buf, 0xAB, TEST_BUFFER_SIZE); // Fill buffer with test pattern

        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            ND2_SGE sge = { m_Buf, static_cast<ULONG>(CHUNK_SIZE), m_pMr->GetLocalToken() };

            if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
                std::cerr << "Write failed for chunk " << (chunk + 1) << "." << std::endl;
                return;
            }

            totalWritten += CHUNK_SIZE;
        }

        for (int i = 0; i < NUM_CHUNKS; i++) {
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
                std::cerr << "WaitForCompletion for chunk " << (i + 1) << " failed." << std::endl;
                return;
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);

        double gbps = CalculateGbps(totalWritten, duration.count());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Written: " << FormatBytes(totalWritten) << std::endl;
        std::cout << "Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "Throughput: " << gbps << " Gbps" << std::endl;

        // Read test (Server -> Client)
        std::cout << "TEST: Read throughput test (Server -> Client)" << std::endl;
        std::cout << "Reading " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE)
                  << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;
        totalWritten = 0;
        startTime = std::chrono::high_resolution_clock::now();

        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            ND2_SGE sge = { m_Buf, static_cast<ULONG>(CHUNK_SIZE), m_pMr->GetLocalToken() };
            if (FAILED(Read(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                std::cerr << "Read failed for chunk " << (chunk + 1) << "." << std::endl;
                return;
            }
            totalWritten += CHUNK_SIZE;
        }

        for (int i = 0; i < NUM_CHUNKS; i++) {
            if (!WaitForCompletionAndCheckContext(READ_CTXT)) {
                std::cerr << "WaitForCompletion for read chunk " << (i + 1) << " failed." << std::endl;
                return;
            }
        }

        endTime = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        gbps = CalculateGbps(totalWritten, duration.count());
        std::cout << "  Results - Read: " << FormatBytes(totalWritten) << std::endl;
        std::cout << "Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "Throughput: " << gbps << " Gbps" << std::endl;
        std::cout << "Read test completed successfully." << std::endl;

        // Notify client that RMA operations are done
        if (FAILED(Send(nullptr, 0, 0, SEND_CTXT))) {
            std::cerr << "Send completion message failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for completion message failed." << std::endl;
            return;
        }

        // Wait for client to finish
        if (FAILED(PostReceive(nullptr, 0, RECV_CTXT))) {
            std::cerr << "PostReceive for client's RMA operations failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for client's RMA operations failed." << std::endl;
            return;
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

        // This flag does not limit remote to read the memory region.
        // Guess write is super-permission of read.
        // It's different for MemoryWindow, see line 305.
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
        // However, MemoryWindow should be specified to allow both read and write separately.
        Bind(m_Buf, TEST_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMw->GetRemoteToken();

        std::cout << "My address: " << myInfo->remoteAddr << ", token: " << myInfo->remoteToken << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for send failed." << std::endl;
            return;
        }

        std::cout << "Peer information sent. Waiting for server's PeerInfo..." << std::endl;

        memset(m_Buf, 0, TEST_BUFFER_SIZE);

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for server's PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for server's PeerInfo failed." << std::endl;
            return;
        }

        std::cout << "Received PeerInfo from server: remoteAddr = " 
                  << reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr
                  << ", remoteToken = " << reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken << std::endl;

        PeerInfo remoteInfo;
        remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
        remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

        memset(m_Buf, 0, TEST_BUFFER_SIZE);

        // Notify Server

        auto now = GetCurrentTimestamp();
        while (GetCurrentTimestamp() - now < 1000000) {
            continue;
        }

        if (FAILED(Send(nullptr, 0, 0, SEND_CTXT))) {
            std::cerr << "Send notification to server failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for notification send failed." << std::endl;
            return;
        }

        // Wait until server runs RMA operations
        if (FAILED(PostReceive(nullptr, 0, RECV_CTXT))) {
            std::cerr << "PostReceive for server's RMA operations failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for server's RMA operations failed." << std::endl;
            return;
        }

        // Write throughput test (Client -> Server)
        std::cout << "TEST: Write throughput test (Client -> Server)" << std::endl;
        std::cout << "Writing " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE)
                    << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;
        size_t totalWritten = 0;
        auto startTime = std::chrono::high_resolution_clock::now();
        memset(m_Buf, 0xAB, TEST_BUFFER_SIZE); // Fill buffer with test pattern

        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            ND2_SGE sge = { m_Buf, static_cast<ULONG>(CHUNK_SIZE), m_pMr->GetLocalToken() };
            if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
                std::cerr << "Write failed for chunk " << (chunk + 1) << "." << std::endl;
                return;
            }

            totalWritten += CHUNK_SIZE;
            //std::cout << "Chunk " << (chunk + 1) << " requested. Total: " << FormatBytes(totalWritten) << std::endl;
        }

        for (int i = 0; i < NUM_CHUNKS; i++) {
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
                std::cerr << "WaitForCompletion for chunk " << (i + 1) << " failed." << std::endl;
                return;
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        double gbps = CalculateGbps(totalWritten, duration.count());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Written: " << FormatBytes(totalWritten) << std::endl;
        std::cout << "Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "Throughput: " << gbps << " Gbps" << std::endl;

        // Read throughput test (Client -> Server)
        std::cout << "TEST: Read throughput test (Client -> Server)" << std::endl;
        std::cout << "Reading " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE)
                    << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;

        totalWritten = 0;
        startTime = std::chrono::high_resolution_clock::now();
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            ND2_SGE sge = { m_Buf, static_cast<ULONG>(CHUNK_SIZE), m_pMr->GetLocalToken() };
            if (FAILED(Read(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                std::cerr << "Read failed for chunk " << (chunk + 1) << "." << std::endl;
                return;
            }

            totalWritten += CHUNK_SIZE;
        }

        for (int i = 0; i < NUM_CHUNKS; i++) {
            if (!WaitForCompletionAndCheckContext(READ_CTXT)) {
                std::cerr << "WaitForCompletion for read chunk " << (i + 1) << " failed." << std::endl;
                return;
            }
        }
        endTime = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        gbps = CalculateGbps(totalWritten, duration.count());
        std::cout << "  Results - Read: " << FormatBytes(totalWritten) << std::endl;
        std::cout << "Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "Throughput: " << gbps << " Gbps" << std::endl;
        std::cout << "Read test completed successfully." << std::endl;

        // Notify server that RMA operations are done
        if (FAILED(Send(nullptr, 0, 0, SEND_CTXT))) {
            std::cerr << "Send completion message failed." << std::endl;
            return;
        }

        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for completion message failed." << std::endl;
            return;
        }

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