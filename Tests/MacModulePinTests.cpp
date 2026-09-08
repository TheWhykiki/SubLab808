#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}
}

int main(int argc, char** argv)
{
    if (argc != 3 || (std::strcmp(argv[1], "pinned") != 0
                     && std::strcmp(argv[1], "unpinned") != 0))
        return fail("usage: MacModulePinTests pinned|unpinned module-path");

    void* hostHandle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (hostHandle == nullptr)
        return fail("fixture module did not load");

    using ValueFunction = int (*)();
    using RetainFunction = const void* (*)();
    using IsRetainedFunction = bool (*)();
    auto value = reinterpret_cast<ValueFunction>(dlsym(hostHandle, "whykikiModulePinFixtureValue"));
    auto retain = reinterpret_cast<RetainFunction>(dlsym(hostHandle, "whykikiRetainModulePinFixture"));
    auto isRetained = reinterpret_cast<IsRetainedFunction>(dlsym(hostHandle, "whykikiModulePinFixtureIsRetained"));
    if (value == nullptr || retain == nullptr || isRetained == nullptr || value() != 808)
        return fail("fixture exports are incomplete");

    const bool pinned = std::strcmp(argv[1], "pinned") == 0;
    const void* retainedToken = nullptr;
    if (pinned)
    {
        constexpr std::size_t threadCount = 16;
        std::vector<const void*> tokens(threadCount);
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (std::size_t index = 0; index < threadCount; ++index)
            threads.emplace_back([&, index]
            {
                const void* firstToken = nullptr;
                bool consistent = true;
                for (int attempt = 0; attempt < 100; ++attempt)
                {
                    const auto* token = retain();
                    if (firstToken == nullptr) firstToken = token;
                    if (token == nullptr || token != firstToken) consistent = false;
                }
                tokens[index] = consistent ? firstToken : nullptr;
            });
        for (auto& thread : threads) thread.join();

        retainedToken = tokens.front();
        for (const auto* token : tokens)
            if (token == nullptr || token != retainedToken)
                return fail("concurrent pin calls did not return one cached status");
        if (! isRetained())
            return fail("pin state was not published before returning");
    }
    else if (isRetained())
        return fail("fixture was retained before the pin operation");

    if (dlclose(hostHandle) != 0)
        return fail("host fixture handle did not close");

    void* probe = dlopen(argv[2], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    if (! pinned)
    {
        if (probe != nullptr)
        {
            dlclose(probe);
            return fail("unpinned control module remained loaded");
        }
        std::puts("PASS: unpinned control module unloaded after its host handle closed.");
        return 0;
    }

    if (probe == nullptr || retainedToken == nullptr)
        return fail("private pin did not keep the module loaded after host close");
    if (value() != 808 || retain() != retainedToken || ! isRetained())
        return fail("retained module code/state was not callable after host close");
    if (dlclose(probe) != 0)
        return fail("probe handle did not close");

    std::puts("PASS: one concurrent, process-long private reference survives host dlclose.");
    return 0;
}
