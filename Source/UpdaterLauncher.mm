#include "UpdaterLauncher.h"
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <unistd.h>

namespace wk {
juce::Result launchNativeUpdater(const juce::String& product, const juce::String& version)
{
    @autoreleasepool
    {
        Dl_info info {};
        if (dladdr(reinterpret_cast<const void*>(&launchNativeUpdater), &info) == 0 || info.dli_fname == nullptr)
            return juce::Result::fail("The installed plugin location could not be determined.");
        NSString* binary = [NSString stringWithUTF8String:info.dli_fname];
        NSString* bundle = [[[binary stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
        NSString* name = [NSString stringWithUTF8String:product.toRawUTF8()];
        NSString* installedVersion = [NSString stringWithUTF8String:version.toRawUTF8()];
        if (binary == nil || name == nil || installedVersion == nil)
            return juce::Result::fail("The plugin update configuration could not be read.");
        NSString* helperName = [name stringByAppendingString:@"Updater.app"];
        NSString* source = [[bundle stringByAppendingPathComponent:@"Contents/Helpers"] stringByAppendingPathComponent:helperName];
        if (![[NSFileManager defaultManager] fileExistsAtPath:source])
            return juce::Result::fail("The updater is missing from this build. Install a complete macOS VST3 release.");
        NSString* temporary = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"WhykikiUpdater-" stringByAppendingString:[[NSUUID UUID] UUIDString]]];
        NSError* error = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtPath:temporary withIntermediateDirectories:NO
                 attributes:@{ NSFilePosixPermissions: @0700 } error:&error])
            return juce::Result::fail(juce::String([[error localizedDescription] UTF8String]));
        NSString* copied = [temporary stringByAppendingPathComponent:helperName];
        if (![[NSFileManager defaultManager] copyItemAtPath:source toPath:copied error:&error])
        {
            [[NSFileManager defaultManager] removeItemAtPath:temporary error:nil];
            return juce::Result::fail(juce::String([[error localizedDescription] UTF8String]));
        }
        // A synchronous launch leaves no callback/thread executing plugin code after the DAW
        // unloads this module. The small helper copy survives the plugin's replacement.
        NSArray* arguments = @[ @"--plugin", bundle, installedVersion,
                                [NSString stringWithFormat:@"%d", getpid()] ];
       #pragma clang diagnostic push
       #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSRunningApplication* launched = [[NSWorkspace sharedWorkspace] launchApplicationAtURL:[NSURL fileURLWithPath:copied]
            options:NSWorkspaceLaunchNewInstance configuration:@{ NSWorkspaceLaunchConfigurationArguments: arguments }
            error:&error];
       #pragma clang diagnostic pop
        if (launched == nil)
        {
            [[NSFileManager defaultManager] removeItemAtPath:temporary error:nil];
            return juce::Result::fail(juce::String([[error localizedDescription] UTF8String]));
        }
        return juce::Result::ok();
    }
}
}
