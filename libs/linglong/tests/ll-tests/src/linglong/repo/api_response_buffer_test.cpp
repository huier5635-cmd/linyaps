// SPDX-FileCopyrightText: 2026 huier
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <gtest/gtest.h>

extern "C" {
#include "include/apiClient.h"
size_t writeDataCallback(void *buffer, size_t size, size_t nmemb, void *userp);
}

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct ResponseBuffer
{
    apiClient_t client{};

    ~ResponseBuffer() { std::free(client.dataReceived); }
};

int callbacks = 0;

void recordCallback(void **, long *)
{
    ++callbacks;
}

void consumeCallback(void **data, long *length)
{
    ++callbacks;
    std::free(*data);
    *data = nullptr;
    *length = 0;
}

TEST(HttpResponseBuffer, AppendsBinaryChunks)
{
    ResponseBuffer buffer;
    std::array<char, 3> first{ 'a', '\0', 'b' };
    std::array<char, 2> second{ 'c', 'd' };
    ASSERT_EQ(writeDataCallback(first.data(), 1, first.size(), &buffer.client), first.size());
    ASSERT_EQ(writeDataCallback(second.data(), second.size(), 1, &buffer.client), second.size());
    ASSERT_EQ(buffer.client.dataReceivedLen, 5);
    EXPECT_EQ(std::memcmp(buffer.client.dataReceived, "a\0bcd\0", 6), 0);
}

TEST(HttpResponseBuffer, CustomCallbackCanConsumeChunks)
{
    ResponseBuffer buffer;
    callbacks = 0;
    buffer.client.data_callback_func = consumeCallback;
    char text[] = "chunk";
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(writeDataCallback(text, 1, 5, &buffer.client), 5U);
        EXPECT_EQ(buffer.client.dataReceived, nullptr);
        EXPECT_EQ(buffer.client.dataReceivedLen, 0);
    }
    EXPECT_EQ(callbacks, 2);
}

TEST(HttpResponseBuffer, ZeroLengthInputRetainsCallbackContract)
{
    ResponseBuffer buffer;
    callbacks = 0;
    buffer.client.data_callback_func = recordCallback;
    char empty[] = "";
    EXPECT_EQ(writeDataCallback(empty, 1, 0, &buffer.client), 0U);
    ASSERT_NE(buffer.client.dataReceived, nullptr);
    EXPECT_EQ(static_cast<char *>(buffer.client.dataReceived)[0], '\0');
    EXPECT_EQ(buffer.client.dataReceivedLen, 0);
    EXPECT_EQ(callbacks, 1);
}

TEST(HttpResponseBuffer, RejectsMultiplicationOverflow)
{
    ResponseBuffer buffer;
    char prefix[] = "old";
    ASSERT_EQ(writeDataCallback(prefix, 1, 3, &buffer.client), 3U);
    void *original = buffer.client.dataReceived;
    callbacks = 0;
    buffer.client.data_callback_func = recordCallback;
    char input[] = "zz";
    EXPECT_EQ(writeDataCallback(input, 2, SIZE_MAX / 2 + 2, &buffer.client), 0U);
    EXPECT_EQ(buffer.client.dataReceived, original);
    EXPECT_EQ(buffer.client.dataReceivedLen, 3);
    EXPECT_EQ(std::memcmp(buffer.client.dataReceived, "old\0", 4), 0);
    EXPECT_EQ(callbacks, 0);
}

int totalOverflowScenario()
{
    ResponseBuffer buffer;
    char prefix[] = "old";
    if (writeDataCallback(prefix, 1, 3, &buffer.client) != 3) {
        return 10;
    }
    void *original = buffer.client.dataReceived;
    buffer.client.dataReceivedLen = LONG_MAX;
    callbacks = 0;
    buffer.client.data_callback_func = recordCallback;
    char input[] = "x";
    const auto result = writeDataCallback(input, 1, 1, &buffer.client);
    return result == 0 && buffer.client.dataReceived == original
        && buffer.client.dataReceivedLen == LONG_MAX && callbacks == 0
      ? 0
      : 1;
}

TEST(HttpResponseBuffer, RejectsUnrepresentableStoredLength)
{
    ASSERT_EXIT(_exit(totalOverflowScenario()), ::testing::ExitedWithCode(0), ".*");
}

int allocationFailureScenario()
{
    ResponseBuffer buffer;
    constexpr long prefixSize = 8 * 1024 * 1024;
    buffer.client.dataReceived = std::malloc(prefixSize + 1);
    if (!buffer.client.dataReceived) {
        return 10;
    }
    std::memset(buffer.client.dataReceived, 'p', prefixSize);
    static_cast<char *>(buffer.client.dataReceived)[prefixSize] = '\0';
    buffer.client.dataReceivedLen = prefixSize;
    void *original = buffer.client.dataReceived;
    callbacks = 0;
    buffer.client.data_callback_func = recordCallback;
    std::array<char, CURL_MAX_WRITE_SIZE> chunk{};

    rlimit previous{};
    if (getrlimit(RLIMIT_AS, &previous) != 0) {
        return 11;
    }
    auto *statm = std::fopen("/proc/self/statm", "r");
    if (!statm) {
        return 12;
    }
    unsigned long pages = 0;
    const int parsed = std::fscanf(statm, "%lu", &pages);
    std::fclose(statm);
    if (parsed != 1) {
        return 13;
    }
    rlimit limited = previous;
    limited.rlim_cur = pages * static_cast<unsigned long>(sysconf(_SC_PAGESIZE));
    if (setrlimit(RLIMIT_AS, &limited) != 0) {
        return 14;
    }
    const auto result = writeDataCallback(chunk.data(), 1, chunk.size(), &buffer.client);
    if (setrlimit(RLIMIT_AS, &previous) != 0) {
        return 15;
    }
    if (result != 0 || buffer.client.dataReceived != original
        || buffer.client.dataReceivedLen != prefixSize || callbacks != 0) {
        return 1;
    }
    const auto *data = static_cast<char *>(buffer.client.dataReceived);
    return std::all_of(data,
                       data + prefixSize,
                       [](char byte) {
                           return byte == 'p';
                       })
        && data[prefixSize] == '\0'
      ? 0
      : 2;
}

TEST(HttpResponseBuffer, AllocationFailurePreservesPartialResponse)
{
    ASSERT_EXIT(_exit(allocationFailureScenario()), ::testing::ExitedWithCode(0), ".*");
}

struct Socket
{
    int fd;

    ~Socket()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

struct CurlEnvironment
{
    CurlEnvironment() { apiClient_setupGlobalEnv(); }

    ~CurlEnvironment() { apiClient_unsetupGlobalEnv(); }
};

TEST(HttpResponseBuffer, NativeClientReceivesBinaryHttpResponse)
{
    CurlEnvironment environment;
    Socket listener{ ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0) };
    ASSERT_GE(listener.fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener.fd, 1), 0);
    socklen_t addressSize = sizeof(address);
    ASSERT_EQ(::getsockname(listener.fd, reinterpret_cast<sockaddr *>(&address), &addressSize), 0);
    const std::string body("body\0end", 8);
    auto served = std::async(std::launch::async, [&] {
        pollfd ready{ listener.fd, POLLIN, 0 };
        if (::poll(&ready, 1, 5000) != 1)
            return false;
        Socket connection{ ::accept(listener.fd, nullptr, nullptr) };
        if (connection.fd < 0)
            return false;
        timeval timeout{ 5, 0 };
        if (::setsockopt(connection.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
            return false;
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 4096) {
            std::array<char, 1024> bytes{};
            const auto count = ::recv(connection.fd, bytes.data(), bytes.size(), 0);
            if (count <= 0)
                return false;
            request.append(bytes.data(), static_cast<size_t>(count));
        }
        if (request.rfind("GET /buffer ", 0) != 0)
            return false;
        const std::string response =
          "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < response.size()) {
            const auto count =
              ::send(connection.fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
            if (count <= 0)
                return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    });
    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    std::unique_ptr<apiClient_t, decltype(&apiClient_free)> client(
      apiClient_create_with_base_path(url.c_str(), nullptr, nullptr),
      apiClient_free);
    ASSERT_NE(client, nullptr);
    apiClient_invoke(client.get(),
                     "/buffer",
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     "GET");
    EXPECT_TRUE(served.get());
    ASSERT_EQ(client->response_code, 200);
    ASSERT_EQ(client->dataReceivedLen, static_cast<long>(body.size()));
    ASSERT_NE(client->dataReceived, nullptr);
    EXPECT_EQ(std::memcmp(client->dataReceived, body.data(), body.size()), 0);
    std::free(client->dataReceived);
    client->dataReceived = nullptr;
}

} // namespace
