#include "UpdaterPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using namespace wk::windows_updater;

    const auto installed = parseVersion("1.4.0");
    const auto newer = parseStableTag("v1.4.1");
    require(installed && newer && isStrictlyNewer(*newer, *installed), "strict stable upgrade");
    require(! parseVersion("01.4.0") && ! parseVersion("1.4") && ! parseVersion("1.4.0.1"),
            "canonical three-part versions only");
    require(parseVersion("255.255.65535") && ! parseVersion("256.0.0")
                && ! parseVersion("0.256.0") && ! parseVersion("0.0.65536"),
            "Windows Installer ProductVersion bounds enforced before download");
    require(! parseStableTag("1.4.1") && ! parseStableTag("v1.4.1-rc1"),
            "stable tags require exact vMAJOR.MINOR.PATCH");
    require(! isStrictlyNewer(*installed, *installed)
                && ! isStrictlyNewer(*parseVersion("1.3.99"), *installed),
            "equal versions and downgrades rejected");

    require(isSafeRepositoryComponent("TheWhykiki") && isSafeRepositoryComponent("SubLab808"),
            "safe repository components");
    require(! isSafeRepositoryComponent("../owner") && ! isSafeRepositoryComponent("owner/repo"),
            "repository path injection rejected");
    require(expectedAssetName("SubLab808", *newer, Architecture::x64)
                == "SubLab808-1.4.1-Windows-x64.msi",
            "exact x64 asset name");
    require(expectedAssetName("SubLab808", *newer, Architecture::arm64ec)
                == "SubLab808-1.4.1-Windows-arm64ec.msi",
            "exact ARM64EC asset name");
    require(expectedAssetUrl("TheWhykiki", "SubLab808", "SubLab808", *newer, Architecture::x64)
                == "https://github.com/TheWhykiki/SubLab808/releases/download/v1.4.1/"
                   "SubLab808-1.4.1-Windows-x64.msi",
            "exact repository, tag and asset URL");
    require(releasesApiUrl("TheWhykiki", "SubLab808")
                == "https://api.github.com/repos/TheWhykiki/SubLab808/releases/latest",
            "exact releases API URL");

    const std::string digest(64, 'a');
    const auto normalized = digestHex("sha256:" + digest);
    require(normalized && *normalized == std::string(64, 'A'), "GitHub sha256 digest normalized");
    require(! digestHex("sha512:" + digest) && ! digestHex("sha256:abcd"),
            "missing or malformed digest rejected");
    require(isAllowedHttpsHost("api.github.com")
                && isAllowedHttpsHost("release-assets.githubusercontent.com"),
            "required GitHub hosts allowed");
    require(! isAllowedHttpsHost("api.github.com.attacker.invalid")
                && ! isAllowedHttpsHost("raw.githubusercontent.com"),
            "redirect host matching is exact");
    require(isCanonicalGuid("DB0CABBA-9411-5738-8A43-98D900748C58")
                && ! isCanonicalGuid("{DB0CABBA-9411-5738-8A43-98D900748C58}"),
            "canonical UpgradeCode format");

    const std::string currentCode = "DB0CABBA-9411-5738-8A43-98D900748C58";
    const std::string otherCode = "8494E96B-8735-5AB6-8E20-D1BF667DADD9";
    const std::vector<MsiUpgradeRow> upgrades {
        { currentCode, "", "1.4.1", "1033", 0x001, "", "WIX_UPGRADE_DETECTED" },
        { currentCode, "1.4.1", "", "1033", 0x002, "", "WIX_DOWNGRADE_DETECTED" },
        { otherCode, "0.0.0", "", "", 0x102, "", "OTHERARCHITECTUREDETECTED" }
    };
    require(hasExactUpgradeContract(upgrades, currentCode, otherCode, *newer),
            "exact WiX 6.0.2 three-row upgrade contract");
    for (std::size_t row = 0; row < upgrades.size(); ++row)
    {
        auto mutant = upgrades;
        mutant[row].attributes ^= 0x200;
        require(! hasExactUpgradeContract(mutant, currentCode, otherCode, *newer),
                "upgrade attribute mutant rejected");
    }
    auto foreignUpgrade = upgrades;
    foreignUpgrade[2].upgradeCode = currentCode;
    require(! hasExactUpgradeContract(foreignUpgrade, currentCode, otherCode, *newer),
            "foreign/duplicate architecture upgrade rows rejected");
    auto removeAll = upgrades;
    removeAll[0].remove = "ALL";
    require(! hasExactUpgradeContract(removeAll, currentCode, otherCode, *newer),
            "non-NULL WiX Upgrade Remove policy rejected");
    require(hasExactLaunchConditions({ "Installed OR NOT WIX_DOWNGRADE_DETECTED",
                                       "(Installed OR NOT OTHERARCHITECTUREDETECTED)" }),
            "exact launch conditions accepted");
    require(! hasExactLaunchConditions({ "1 OR NOT WIX_DOWNGRADE_DETECTED",
                                         "Installed OR NOT OTHERARCHITECTUREDETECTED" }),
            "bypassed downgrade condition rejected");
    require(isForbiddenMsiSideEffectTable("CustomAction")
                && isForbiddenMsiSideEffectTable("ServiceInstall")
                && isForbiddenMsiSideEffectTable("MoveFiles")
                && ! isForbiddenMsiSideEffectTable("File"),
            "dangerous MSI side-effect table policy");

    const std::map<std::string, std::string> directories {
        { "Contents", "INSTALLFOLDER" }, { "Resources", "Contents" },
        { "Binary", "Contents" }, { "INSTALLFOLDER", "VST3Folder" },
        { "VST3Folder", "TARGETDIR" }, { "WindowsFolder", "TARGETDIR" },
        { "TARGETDIR", "" }
    };
    require(componentDirectoriesAreInsideInstallFolder(directories, { "Resources", "Binary" }),
            "all payload components descend from INSTALLFOLDER");
    require(! componentDirectoriesAreInsideInstallFolder(directories, { "Resources", "WindowsFolder" }),
            "component escaping to WindowsFolder rejected");
    auto cycle = directories;
    cycle["A"] = "B";
    cycle["B"] = "A";
    require(! componentDirectoriesAreInsideInstallFolder(cycle, { "Resources", "Binary" }),
            "disconnected directory cycle rejected");
    auto dangling = directories;
    dangling["Unused"] = "MissingParent";
    require(! componentDirectoriesAreInsideInstallFolder(dangling, { "Resources", "Binary" }),
            "dangling directory parent rejected");

    std::cout << "PASS: Windows updater portable policy\n";
    return 0;
}
