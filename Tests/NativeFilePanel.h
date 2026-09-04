#pragma once
#include <memory>
#include <string>

// Test-only observer for panels owned by this process. Retaining a panel keeps
// observation safe after JUCE closes/releases it; no other app is inspected.
class NativeFilePanel final
{
public:
    static void prepareTestApplication();
    static std::unique_ptr<NativeFilePanel> findVisible(bool importing, const char* title);
    static int visibleCount();
    ~NativeFilePanel();
    NativeFilePanel(const NativeFilePanel&) = delete;
    NativeFilePanel& operator=(const NativeFilePanel&) = delete;
    bool isVisible() const;
    bool hasDelegate() const;
    std::string className() const;
    void useFixtureLocation(const std::string& directory, const std::string& filename);
private:
    explicit NativeFilePanel(void*);
    void* panel = nullptr;
};
