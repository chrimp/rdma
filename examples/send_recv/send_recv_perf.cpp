#include "NDSession.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <iomanip>
#include <thread>

#undef max
#undef min

#define TEST_THROUGHPUT

constexpr char TEST_PORT[] = "54321";

// Performance test constants
constexpr size_t CHUNK_SIZE = 655350000;
constexpr size_t TEST_BUFFER_SIZE = std::max(536870912ULL, CHUNK_SIZE); // At least 512MB or chunk size

constexpr size_t maxSge = 32; // Temporary value for deciding test size
constexpr size_t THROUGHPUT_TEST_SIZE = CHUNK_SIZE * maxSge;

constexpr int NUM_CHUNKS = static_cast<int>(THROUGHPUT_TEST_SIZE / CHUNK_SIZE);  // 20 chunks
constexpr size_t RTT_TEST_SIZE = 1;  // Small size for RTT test
constexpr int RTT_TEST_ITERATIONS = 10;  // Number of ping-pong iterations

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define read_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

struct PeerInfo {
    UINT64 remoteAddr;
    UINT32 remoteToken;
};

// Utility functions for performance measurement
uint64_t GetCurrentTimestamp() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

double CalculateGbps(uint64_t bytes, uint64_t nanoseconds) {
    return (static_cast<double>(bytes) * 8.0) / (static_cast<double>(nanoseconds) / 1e9) / 1e9;
}

double CalculateLatencyMicroseconds(uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1000.0;
}

std::string FormatBytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        return std::to_string(bytes / (1024ULL * 1024 * 1024 * 1024)) + "TB";
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        return std::to_string(bytes / (1024ULL * 1024 * 1024)) + "GB";
    } else if (bytes >= 1024ULL * 1024) {
        return std::to_string(bytes / (1024ULL * 1024)) + "MB";
    } else if (bytes >= 1024ULL) {
        return std::to_string(bytes / 1024ULL) + "KB";
    } else {
        return std::to_string(bytes) + "B";
    }
}

void ShowUsage() {
    printf("rdma_perf.exe [options]\n"
           "Options:\n"
           "\t-s <local_ip>           - Start as server\n"
           "\t-c <local_ip> <server_ip> - Start as client\n"
           "\nThe program automatically runs all performance tests:\n"
           "\t1. Basic connectivity test (existing)\n"
           "\t2. Throughput Send Test (client->server, %dx%lluGB chunks)\n"
           "\t3. Throughput Receive Test (server->client, %dx%lluGB chunks)\n"
           "\t4. Round-Trip Time Test (%d iterations)\n"
           "\nBuffer size: %lluMB static allocation\n",
           NUM_CHUNKS, CHUNK_SIZE / (1024ULL*1024*1024),
           NUM_CHUNKS, CHUNK_SIZE / (1024ULL*1024*1024),
           RTT_TEST_ITERATIONS,
           TEST_BUFFER_SIZE / (1024ULL*1024));
}

// MARK: TestServer
class TestServer : public NDSessionServerBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        std::cout << "Max transfer length: " << info.MaxTransferLength << std::endl;
        std::cout << "Max send sge: " << info.MaxInitiatorSge << std::endl;
        std::cout << "Send/Write threshold: " << info.LargeRequestThreshold << std::endl;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(static_cast<DWORD>(TEST_BUFFER_SIZE), flags))) return false;

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

        ND2_SGE sge = { m_Buf, static_cast<ULONG>(TEST_BUFFER_SIZE), m_pMr->GetLocalToken() };

        #ifdef TEST_THROUGHPUT
        // ==================== PERFORMANCE TESTS START ====================
        std::cout << "\n================================================" << std::endl;
        std::cout << "STARTING RDMA PERFORMANCE TESTS - SERVER SIDE" << std::endl;
        std::cout << "================================================\n" << std::endl;

        // Test 1: Throughput Receive Test (Client -> Server)
        std::cout << "TEST 1: Throughput Receive Test (Client -> Server)" << std::endl;
        std::cout << "Receiving " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE) << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;

        auto startTime = std::chrono::high_resolution_clock::now();

        // First loop: Batch-post all receives
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            sge = { m_Buf, static_cast<ULONG>(CHUNK_SIZE), m_pMr->GetLocalToken() };
            
            if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                std::cerr << "PostReceive failed in throughput test." << std::endl;
                #ifdef _DEBUG
                abort();
                #endif
                return;
            }
        }

        // Second loop: Wait for all completions
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
                std::cerr << "WaitForCompletion failed in throughput test." << std::endl;
                return;
            }
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        
        double gbps = CalculateGbps(THROUGHPUT_TEST_SIZE, duration.count());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Received: " << THROUGHPUT_TEST_SIZE << " bytes (" << (THROUGHPUT_TEST_SIZE / (1024*1024)) << "MB)" << std::endl;
        std::cout << "  Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "  Throughput: " << gbps << " Gbps" << std::endl;

        CreateMW();
        Bind(m_Buf, static_cast<DWORD>(TEST_BUFFER_SIZE), ND_OP_FLAG_ALLOW_WRITE);

        // Test 2: Throughput Send Test (Server -> Client)
        std::cout << "TEST 2: Throughput Send Test (Server -> Client)" << std::endl;
        std::cout << "Sending " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE) << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;

        uint64_t totalSent = 0;
        startTime = std::chrono::high_resolution_clock::now();

        // Only set memory once
        memset(m_Buf, 0xAB, TEST_BUFFER_SIZE);

        std::string chunkSizeStr = FormatBytes(CHUNK_SIZE);
        
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            std::cout << "  Sending chunk " << (chunk + 1) << "/" << NUM_CHUNKS << " (" << chunkSizeStr << ")..." << std::endl;
            
            uint64_t chunkSent = 0;
            while (chunkSent < CHUNK_SIZE) {
                size_t sendSize = std::min(static_cast<size_t>(TEST_BUFFER_SIZE), static_cast<size_t>(CHUNK_SIZE - chunkSent));
                
                // Use SGE to control actual transfer size, not buffer size
                sge = { m_Buf, static_cast<ULONG>(sendSize), m_pMr->GetLocalToken() };
                
                if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
                    std::cerr << "Send failed in throughput test." << std::endl;
                    return;
                }
                if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
                    std::cerr << "WaitForCompletion failed in throughput test." << std::endl;
                    return;
                }
                
                chunkSent += sendSize;
                totalSent += sendSize;
            }
            
            std::cout << "    Chunk " << (chunk + 1) << " completed. Total sent: " << FormatBytes(totalSent) << std::endl;
        }
        
        endTime = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        
        gbps = CalculateGbps(totalSent, duration.count());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Sent: " << totalSent << " bytes (" << (totalSent / (1024*1024*1024)) << " GB)" << std::endl;
        std::cout << "  Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "  Throughput: " << gbps << " Gbps" << std::endl;
        #endif // TEST_THROUGHPUT

        // Test 3: RTT Test (Server responds to pings)
        std::cout << "\nTEST 3: Round-Trip Time Test (Server responding to pings)" << std::endl;
        std::cout << "Responding to " << RTT_TEST_ITERATIONS << " ping-pong iterations..." << std::endl;

        // Two postreceives to create some room

        sge = { m_Buf, static_cast<ULONG>(RTT_TEST_SIZE), m_pMr->GetLocalToken() };
        HRESULT one = PostReceive(&sge, 1, RECV_CTXT);
        HRESULT two = PostReceive(&sge, 1, RECV_CTXT);
        if (FAILED(one) || FAILED(two)) {
            std::cerr << "PostReceive failed in RTT test." << std::endl;
            return;
        }
        
        for (uint32_t i = 0; i < RTT_TEST_ITERATIONS; i++) {
            std::cout << "  Waiting for ping " << (i + 1) << "/" << RTT_TEST_ITERATIONS << "..." << std::endl;
            
            // Receive ping from client
            sge = { m_Buf, static_cast<ULONG>(RTT_TEST_SIZE), m_pMr->GetLocalToken() };
            
            if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                std::cerr << "PostReceive failed in RTT test." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
                std::cerr << "WaitForCompletion failed in RTT test." << std::endl;
                return;
            }
            
            //std::cout << "  Received ping " << (i + 1) << ", sending pong back..." << std::endl;
            
            // Send pong back to client
            if (FAILED(Send(&sge, 1, ND_OP_FLAG_INLINE, SEND_CTXT))) {
                std::cerr << "Send failed in RTT test." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
                std::cerr << "WaitForCompletion failed in RTT test." << std::endl;
                return;
            }
            
            std::cout << "  Ping-pong " << (i + 1) << " completed." << std::endl;
        }
        
        std::cout << "  RTT test completed on server side" << std::endl;

        std::cout << "\n================================================" << std::endl;
        std::cout << "ALL PERFORMANCE TESTS COMPLETED - SERVER SIDE" << std::endl;
        std::cout << "================================================" << std::endl;

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
        if (FAILED(RegisterDataBuffer(static_cast<DWORD>(TEST_BUFFER_SIZE), flags))) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    void Run(const char* localAddr, const char* serverAddr) {
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
        ND2_SGE sge = { m_Buf, static_cast<ULONG>(TEST_BUFFER_SIZE), m_pMr->GetLocalToken() };

        #ifdef TEST_THROUGHPUT
        // ==================== PERFORMANCE TESTS START ====================
        std::cout << "\n================================================" << std::endl;
        std::cout << "STARTING RDMA PERFORMANCE TESTS - CLIENT SIDE" << std::endl;
        std::cout << "================================================\n" << std::endl;

        // Test 1: Throughput Send Test (Client -> Server)
        std::cout << "TEST 1: Throughput Send Test (Client -> Server)" << std::endl;
        std::cout << "Sending " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE) << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;

        uint64_t totalSent = 0;
        auto startTime = std::chrono::high_resolution_clock::now();

        // Only set memory once
        memset(m_Buf, 0xAB, TEST_BUFFER_SIZE);
        std::string chunkSizeStr = FormatBytes(CHUNK_SIZE);
        
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            std::cout << "  Sending chunk " << (chunk + 1) << "/" << NUM_CHUNKS << " (" << chunkSizeStr << ")..." << std::endl;
            
            uint64_t chunkSent = 0;
            while (chunkSent < CHUNK_SIZE) {
                size_t sendSize = std::min(static_cast<size_t>(TEST_BUFFER_SIZE), static_cast<size_t>(CHUNK_SIZE - chunkSent));
                
                // Use SGE to control actual transfer size, not buffer size
                sge = { m_Buf, static_cast<ULONG>(sendSize), m_pMr->GetLocalToken() };
                
                if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
                    std::cerr << "Send failed in throughput test." << std::endl;
                    return;
                }
                if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
                    std::cerr << "WaitForCompletion failed in throughput test." << std::endl;
                    return;
                }
                
                chunkSent += sendSize;
                totalSent += sendSize;
            }
            
            std::cout << "    Chunk " << (chunk + 1) << " completed. Total sent: " << FormatBytes(totalSent) << std::endl;
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        
        double gbps = CalculateGbps(totalSent, duration.count());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Sent: " << totalSent << " bytes (" << (totalSent / (1024*1024*1024)) << " GB)" << std::endl;
        std::cout << "  Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "  Throughput: " << gbps << " Gbps" << std::endl;

        // Test 2: Throughput Receive Test (Server -> Client)
        std::cout << "TEST 2: Throughput Receive Test (Server -> Client)" << std::endl;
        std::cout << "Receiving " << NUM_CHUNKS << " chunks of " << FormatBytes(CHUNK_SIZE) << " each (total: " << FormatBytes(THROUGHPUT_TEST_SIZE) << ")..." << std::endl;
        
        startTime = std::chrono::high_resolution_clock::now();

        // First loop: Batch-post all receives
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            sge = { m_Buf, static_cast<ULONG>(CHUNK_SIZE), m_pMr->GetLocalToken() };
            
            if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                std::cerr << "PostReceive failed in throughput test." << std::endl;
                #ifdef _DEBUG
                abort();
                #endif
                return;
            }
        }

        // Second loop: Wait for all completions
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
                std::cerr << "WaitForCompletion failed in throughput test." << std::endl;
                return;
            }
        }
        
        endTime = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
        
        gbps = CalculateGbps(THROUGHPUT_TEST_SIZE, duration.count());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Received: " << THROUGHPUT_TEST_SIZE << " bytes (" << (THROUGHPUT_TEST_SIZE / (1024*1024*1024)) << " GB)" << std::endl;
        std::cout << "  Duration: " << (duration.count() / 1e9) << " seconds" << std::endl;
        std::cout << "  Throughput: " << gbps << " Gbps" << std::endl;
        #endif // TEST_THROUGHPUT

        // Test 3: RTT Test (Client initiates pings)
        std::cout << "\nTEST 3: Round-Trip Time Test (Client initiating pings)" << std::endl;
        std::cout << "Running " << RTT_TEST_ITERATIONS << " ping-pong iterations..." << std::endl;
        
        std::vector<uint64_t> rttMeasurements;
        rttMeasurements.reserve(RTT_TEST_ITERATIONS);

        // PostReceive twice to create some room
        sge = { m_Buf, static_cast<ULONG>(RTT_TEST_SIZE), m_pMr->GetLocalToken() };
        HRESULT one = PostReceive(&sge, 1, RECV_CTXT);
        HRESULT two = PostReceive(&sge, 1, RECV_CTXT);
        if (FAILED(one) || FAILED(two)) {
            std::cerr << "PostReceive failed in RTT test." << std::endl;
            return;
        }

        // Hold 100ms
        auto now = std::chrono::high_resolution_clock::now();
        while (std::chrono::high_resolution_clock::now() - now < std::chrono::milliseconds(100)) {
            _mm_pause();
        }
        
        for (uint32_t i = 0; i < RTT_TEST_ITERATIONS; i++) {
            std::cout << "  Starting ping-pong " << (i + 1) << "/" << RTT_TEST_ITERATIONS << "..." << std::endl;
            
            // Fill buffer with test pattern
            memset(m_Buf, 0xEF, RTT_TEST_SIZE);
            sge = { m_Buf, static_cast<ULONG>(RTT_TEST_SIZE), m_pMr->GetLocalToken() };
            
            auto rttStart = std::chrono::high_resolution_clock::now();
            
            // Send ping to server
            if (FAILED(Send(&sge, 1, ND_OP_FLAG_INLINE, SEND_CTXT))) {
                std::cerr << "Send failed in RTT test." << std::endl;
                return;
            }
            if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
              std::cerr << "PostReceive failed in RTT test." << std::endl;
              return;
            }
            if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
                std::cerr << "WaitForCompletion failed in RTT test." << std::endl;
                return;
            }
            
            //std::cout << "  Ping " << (i + 1) << " sent, waiting for pong..." << std::endl;
            
            // Receive pong from server

            if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
                std::cerr << "WaitForCompletion failed in RTT test." << std::endl;
                return;
            }
            
            auto rttEnd = std::chrono::high_resolution_clock::now();
            auto rttTime = std::chrono::duration_cast<std::chrono::nanoseconds>(rttEnd - rttStart);
            rttMeasurements.push_back(rttTime.count());
            
            std::cout << "  Ping-pong " << (i + 1) << " completed. RTT: " << CalculateLatencyMicroseconds(rttTime.count()) << " μs." << std::endl;
        }
        
        // Calculate RTT statistics
        double totalRTT = 0;
        uint64_t minRTT = UINT64_MAX;
        uint64_t maxRTT = 0;
        
        for (uint64_t rtt : rttMeasurements) {
            totalRTT += rtt;
            minRTT = std::min(minRTT, rtt);
            maxRTT = std::max(maxRTT, rtt);
        }
        
        double avgRTT = totalRTT / rttMeasurements.size();
        
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Results - Iterations: " << RTT_TEST_ITERATIONS << std::endl;
        std::cout << "  Average RTT: " << CalculateLatencyMicroseconds(avgRTT) << " μs" << std::endl;
        std::cout << "  Min RTT: " << CalculateLatencyMicroseconds(minRTT) << " μs" << std::endl;
        std::cout << "  Max RTT: " << CalculateLatencyMicroseconds(maxRTT) << " μs" << std::endl;

        std::cout << "\n================================================" << std::endl;
        std::cout << "ALL PERFORMANCE TESTS COMPLETED - CLIENT SIDE" << std::endl;
        std::cout << "================================================" << std::endl;

        Shutdown();
    }
};

// NOTE: This function appears to be unused in the main flow but keeping for potential future use
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

    std::cout << "RDMA Performance Test Suite" << std::endl;
    std::cout << "Throughput test: " << NUM_CHUNKS << " chunks of " << (CHUNK_SIZE / (1024ULL*1024*1024)) << "GB each (total: " << (THROUGHPUT_TEST_SIZE / (1024ULL*1024*1024)) << " GB)" << std::endl;
    std::cout << "RTT test: " << RTT_TEST_ITERATIONS << " iterations, " << RTT_TEST_SIZE << " bytes per message\n" << std::endl;

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