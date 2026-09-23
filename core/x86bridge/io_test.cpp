#include "bridge_io.h"

#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    const std::wstring name = L"\\\\.\\pipe\\amd-nr-x86bridge-io-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());

    x86bridge::Handle server(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr));
    assert(server);

    x86bridge::Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    assert(client);

    if (!ConnectNamedPipe(server.value, nullptr))
        assert(GetLastError() == ERROR_PIPE_CONNECTED);

    x86bridge::Handle self(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    assert(self);

    unsigned char sent = 0x5a, received = 0;
    assert(x86bridge::Send(client.value, self.value, &sent, sizeof(sent), 1000));
    assert(x86bridge::Receive(server.value, self.value, &received, sizeof(received), 1000));
    assert(received == sent);

    const uint64_t started = GetTickCount64();
    const bool timeoutFailed = !x86bridge::Receive(
        server.value, self.value, &received, sizeof(received), 50);
    const DWORD timeoutError = GetLastError();
    assert(timeoutFailed);
    if (timeoutError != ERROR_TIMEOUT)
        std::fprintf(stderr, "unexpected timeout error=%lu\n", timeoutError);
    assert(timeoutError == ERROR_TIMEOUT);
    assert(GetTickCount64() - started < 2000);

    std::puts("PASS native named-pipe transfer and bounded timeout");
}
