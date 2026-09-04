#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <vector>
#include <cmath>
#include <limits>
#include <locale>

namespace wk
{
struct Preset
{
    juce::String id, name, category, description;
    std::map<juce::String, float> values;
    int factoryIndex = -1;
};

// File operations are explicit UI/control-thread actions. Construction and the audio
// callback never touch the filesystem. Each product owns its directory and file type.
class PresetLibrary
{
public:
    PresetLibrary(juce::AudioProcessor& owner, juce::AudioProcessorValueTreeState& state,
                  juce::String productName, const std::vector<Preset>& factoryBank,
                  juce::File storage = {})
        : processor(owner), parameters(state), product(std::move(productName)), factory(factoryBank),
          directory(storage == juce::File() ? juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
              .getChildFile("Whykiki Audio").getChildFile(product).getChildFile("Presets") : storage),
          fileLock("WhykikiPresets-" + juce::String(directory.getFullPathName().hashCode64())) {}

    // Use an explicit Unicode-capable ctype facet, never setlocale(): a plugin must
    // not change the DAW's process-wide locale to compare names containing umlauts.
    static juce::String searchKey(const juce::String& text)
    {
        static const auto textLocale = [] {
            for (const auto* name : { "en_US.UTF-8", "C.UTF-8", "" })
                try { return std::locale(name); } catch (const std::runtime_error&) {}
            return std::locale::classic();
        }();
        const auto& facet = std::use_facet<std::ctype<wchar_t>>(textLocale);
        juce::String key;
        auto cursor = text.getCharPointer();
        while (! cursor.isEmpty())
            key += juce::String::charToString(static_cast<juce::juce_wchar>(facet.tolower(static_cast<wchar_t>(cursor.getAndAdvance()))));
        return key;
    }

    const std::vector<Preset>& factoryPresets() const { return factory; }
    juce::File storageDirectory() const { return directory; }
    juce::String extension() const { return "." + product.toLowerCase() + "preset"; }

    std::vector<Preset> userPresets() const
    {
        std::vector<Preset> result;
        for (const auto& file : directory.findChildFiles(juce::File::findFiles, false, "*" + extension()))
        {
            Preset preset;
            if (readFile(file, preset).wasOk() && file.getFileNameWithoutExtension() == preset.id)
                result.push_back(std::move(preset));
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return searchKey(a.name) < searchKey(b.name);
        });
        return result;
    }

    Preset current() const
    {
        {
            const juce::ScopedLock lock(selectionLock);
            if (selected.id.isNotEmpty()) return selected;
        }
        const auto index = juce::jlimit(0, static_cast<int>(factory.size()) - 1, processor.getCurrentProgram());
        return factory[static_cast<size_t>(index)];
    }

    bool isModified() const
    {
        const auto preset = current();
        for (const auto& [id, expected] : preset.values)
        {
            const auto* parameter = parameters.getParameter(id);
            const auto* raw = parameters.getRawParameterValue(id);
            if (parameter == nullptr || raw == nullptr) return true;
            const auto& range = parameter->getNormalisableRange();
            const auto legal = range.snapToLegalValue(expected);
            // A fixed normalised epsilon would hide single-step changes to large
            // integer ranges such as ReverseLab's random seed (1..999999).
            const auto tolerance = juce::jmax(0.000001f, range.interval * 0.25f,
                std::abs(legal) * std::numeric_limits<float>::epsilon() * 2.0f);
            if (std::abs(raw->load() - legal) > tolerance) return true;
        }
        return false;
    }

    // Called inside the processor's existing program/state transaction.
    void clearSelection() { const juce::ScopedLock lock(selectionLock); selected = {}; }
    void appendSelection(juce::ValueTree& state) const
    {
        state.removeChild(state.getChildWithName("WkPresetSelection"), nullptr);
        const juce::ScopedLock lock(selectionLock);
        if (selected.id.isNotEmpty()) state.addChild(selectionTree(selected), -1, nullptr);
    }
    void restoreSelection(const juce::ValueTree& state)
    {
        Preset candidate;
        const auto tree = state.getChildWithName("WkPresetSelection");
        if (tree.isValid())
        {
            candidate.id = tree["id"].toString(); candidate.name = tree["name"].toString();
            candidate.category = tree["category"].toString(); candidate.description = tree["description"].toString();
            for (const auto& child : tree)
                candidate.values[child["id"].toString()] = static_cast<float>(child["value"]);
            if (! validId(candidate.id) || candidate.name.isEmpty() || validateValues(candidate).failed()) candidate = {};
        }
        const juce::ScopedLock lock(selectionLock);
        selected = std::move(candidate);
    }

    juce::Result saveAs(const juce::String& name, const juce::String& category)
    {
        if (auto result = validateName(name); result.failed()) return result;
        if (! fileLock.enter(2000)) return busy();
        const juce::ScopeGuard unlock { [this] { fileLock.exit(); } };
        if (nameExists(name.trim())) return juce::Result::fail("A user preset with this name already exists. Choose another name.");
        auto preset = capture();
        preset.id = juce::Uuid().toString(); preset.name = name.trim();
        preset.category = category.trim().substring(0, 48); preset.factoryIndex = -1;
        if (preset.category.isEmpty()) preset.category = "User";
        if (auto result = writeFile(pathFor(preset.id), preset); result.failed()) return result;
        select(preset);
        return juce::Result::ok();
    }

    juce::Result save()
    {
        const auto existing = current();
        if (existing.factoryIndex >= 0) return juce::Result::fail("Use Save As to create your own copy of a factory preset.");
        if (! fileLock.enter(2000)) return busy();
        const juce::ScopeGuard unlock { [this] { fileLock.exit(); } };
        Preset disk;
        if (auto result = readFile(pathFor(existing.id), disk); result.failed()) return result;
        // A project remembers its own baseline. Do not overwrite edits saved by another
        // instance since this instance loaded the preset.
        if (disk.values != existing.values)
            return juce::Result::fail("This preset was changed in another instance. Use Save As or reload it first.");
        auto preset = capture();
        preset.id = disk.id; preset.name = disk.name; preset.category = disk.category;
        preset.description = disk.description; preset.factoryIndex = -1;
        if (auto result = writeFile(pathFor(preset.id), preset); result.failed()) return result;
        select(preset);
        return juce::Result::ok();
    }

    juce::Result load(const Preset& requested)
    {
        if (requested.factoryIndex >= 0)
        {
            if (! juce::isPositiveAndBelow(requested.factoryIndex, static_cast<int>(factory.size())))
                return juce::Result::fail("Unknown factory preset.");
            processor.setCurrentProgram(requested.factoryIndex);
            changed();
            return juce::Result::ok();
        }
        if (! validId(requested.id)) return juce::Result::fail("Invalid preset identity.");
        Preset preset;
        if (auto result = readFile(pathFor(requested.id), preset); result.failed()) return result;
        auto state = snapshot();
        if (! state.isValid()) return juce::Result::fail("Could not capture the current plugin state.");
        for (auto child : state)
        {
            const auto id = child["id"].toString();
            if (const auto it = preset.values.find(id); it != preset.values.end())
                child.setProperty("value", it->second, nullptr);
        }
        state.removeChild(state.getChildWithName("WkPresetSelection"), nullptr);
        state.addChild(selectionTree(preset), -1, nullptr);
        juce::MemoryBlock data;
        juce::AudioProcessor::copyXmlToBinary(*state.createXml(), data);
        processor.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
        changed();
        return juce::Result::ok();
    }

    juce::Result renameCurrent(const juce::String& name)
    {
        if (auto result = validateName(name); result.failed()) return result;
        const auto active = current();
        if (active.factoryIndex >= 0) return juce::Result::fail("Factory presets cannot be renamed.");
        if (! fileLock.enter(2000)) return busy();
        const juce::ScopeGuard unlock { [this] { fileLock.exit(); } };
        if (nameExists(name.trim(), active.id)) return juce::Result::fail("A user preset with this name already exists.");
        Preset disk;
        if (auto result = readFile(pathFor(active.id), disk); result.failed()) return result;
        disk.name = name.trim();
        if (auto result = writeFile(pathFor(disk.id), disk); result.failed()) return result;
        auto renamed = active; renamed.name = disk.name; // Preserve the in-memory dirty baseline.
        select(renamed);
        return juce::Result::ok();
    }

    juce::Result deleteCurrent()
    {
        const auto active = current();
        if (active.factoryIndex >= 0) return juce::Result::fail("Factory presets cannot be deleted.");
        if (! fileLock.enter(2000)) return busy();
        const juce::ScopeGuard unlock { [this] { fileLock.exit(); } };
        if (! pathFor(active.id).existsAsFile() || ! pathFor(active.id).deleteFile())
            return juce::Result::fail("Could not delete the user preset.");
        favouritePath(active.id).deleteFile();
        // Keep the current sound and its identity in the project; Save As can recover it.
        changed();
        return juce::Result::ok();
    }

    juce::Result importPreset(const juce::File& file, Preset& imported)
    {
        Preset preset;
        if (auto result = readFile(file, preset); result.failed()) return result;
        if (! fileLock.enter(2000)) return busy();
        const juce::ScopeGuard unlock { [this] { fileLock.exit(); } };
        const auto baseName = preset.name;
        for (int suffix = 2; nameExists(preset.name); ++suffix)
            preset.name = baseName.substring(0, 65) + " (" + juce::String(suffix) + ")";
        preset.id = juce::Uuid().toString(); preset.factoryIndex = -1;
        if (auto result = writeFile(pathFor(preset.id), preset); result.failed()) return result;
        imported = std::move(preset);
        return juce::Result::ok();
    }
    juce::Result exportCurrent(const juce::File& file) const
    {
        auto preset = capture();
        if (preset.factoryIndex >= 0) preset.id = juce::Uuid().toString();
        preset.factoryIndex = -1;
        return writeFile(file, preset);
    }

    bool isFavourite(const juce::String& id) const { return safeIdentity(id) && favouritePath(id).existsAsFile(); }
    juce::Result setFavourite(const juce::String& id, bool favourite)
    {
        if (! safeIdentity(id)) return juce::Result::fail("Invalid preset identity.");
        if (! fileLock.enter(2000)) return busy();
        const juce::ScopeGuard unlock { [this] { fileLock.exit(); } };
        const auto file = favouritePath(id);
        if (favourite)
        {
            if (auto result = file.getParentDirectory().createDirectory(); result.failed()) return result;
            return file.replaceWithText("1") ? juce::Result::ok() : juce::Result::fail("Could not save favourite.");
        }
        return ! file.exists() || file.deleteFile() ? juce::Result::ok() : juce::Result::fail("Could not remove favourite.");
    }

private:
    static juce::Result busy() { return juce::Result::fail("The preset library is busy. Please try again."); }
    static bool validId(const juce::String& id)
    {
        return id.length() == 32 && id.containsOnly("0123456789abcdefABCDEF");
    }
    static bool safeIdentity(const juce::String& id)
    {
        return validId(id) || (id.startsWith("factory-") && id.substring(8).isNotEmpty()
                               && id.substring(8).containsOnly("0123456789"));
    }
    juce::File pathFor(const juce::String& id) const { return directory.getChildFile(id + extension()); }
    juce::File favouritePath(const juce::String& id) const { return directory.getChildFile("Favourites").getChildFile(id + ".fav"); }
    static juce::Result validateName(const juce::String& name)
    {
        const auto trimmed = name.trim();
        if (trimmed.isEmpty() || trimmed.length() > 80 || trimmed.containsAnyOf("\r\n\t"))
            return juce::Result::fail("Enter a preset name with 1 to 80 characters on one line.");
        return juce::Result::ok();
    }
    bool nameExists(const juce::String& name, const juce::String& exceptId = {}) const
    {
        for (const auto& preset : userPresets())
            if (preset.id != exceptId && searchKey(preset.name) == searchKey(name)) return true;
        return false;
    }
    juce::ValueTree snapshot() const
    {
        juce::MemoryBlock data; processor.getStateInformation(data);
        if (auto xml = juce::AudioProcessor::getXmlFromBinary(data.getData(), static_cast<int>(data.getSize())))
            return juce::ValueTree::fromXml(*xml);
        return {};
    }
    Preset capture() const
    {
        auto preset = current(); preset.values.clear();
        for (const auto& child : snapshot())
        {
            const auto id = child["id"].toString();
            if (parameters.getParameter(id) != nullptr) preset.values[id] = static_cast<float>(child["value"]);
        }
        return preset;
    }
    juce::Result validateValues(const Preset& preset) const
    {
        if (preset.values.size() != factory.front().values.size())
            return juce::Result::fail("The preset does not contain every parameter.");
        for (const auto& [id, value] : preset.values)
        {
            const auto* parameter = parameters.getParameter(id);
            if (parameter == nullptr || ! std::isfinite(value)) return juce::Result::fail("Unknown or invalid preset parameter: " + id);
            const auto& range = parameter->getNormalisableRange();
            if (value < range.start - 0.0001f || value > range.end + 0.0001f)
                return juce::Result::fail("Preset parameter is outside its supported range: " + id);
        }
        return juce::Result::ok();
    }
    juce::Result readFile(const juce::File& file, Preset& preset) const
    {
        if (! file.existsAsFile() || file.getSize() > 256 * 1024)
            return juce::Result::fail("Preset file is missing or too large.");
        juce::var json;
        if (auto result = juce::JSON::parse(file.loadFileAsString(), json); result.failed()) return result;
        if (! json.isObject() || json["format"].toString() != "WhykikiPreset"
            || ! json["version"].isInt() || static_cast<int>(json["version"]) != 1
            || json["plugin"].toString() != product)
            return juce::Result::fail("This file is not a supported " + product + " preset.");
        preset = {}; preset.id = json["id"].toString(); preset.name = json["name"].toString();
        preset.category = json["category"].toString(); preset.description = json["description"].toString();
        if (! validId(preset.id)) return juce::Result::fail("Invalid preset identity.");
        if (auto result = validateName(preset.name); result.failed()) return result;
        if (preset.category.length() > 48 || preset.description.length() > 1024)
            return juce::Result::fail("Preset metadata is too long.");
        if (auto* values = json["parameters"].getDynamicObject())
            for (const auto& property : values->getProperties())
            {
                if (! property.value.isDouble() && ! property.value.isInt() && ! property.value.isInt64())
                    return juce::Result::fail("Preset parameters must be numbers.");
                preset.values[property.name.toString()] = static_cast<float>(property.value);
            }
        return validateValues(preset);
    }
    juce::Result writeFile(const juce::File& file, const Preset& preset) const
    {
        if (auto result = validateValues(preset); result.failed()) return result;
        auto root = std::make_unique<juce::DynamicObject>();
        root->setProperty("format", "WhykikiPreset"); root->setProperty("version", 1);
        root->setProperty("plugin", product); root->setProperty("id", preset.id);
        root->setProperty("name", preset.name); root->setProperty("category", preset.category);
        root->setProperty("description", preset.description);
        auto values = std::make_unique<juce::DynamicObject>();
        for (const auto& [id, value] : preset.values) values->setProperty(id, value);
        root->setProperty("parameters", juce::var(values.release()));
        if (auto result = file.getParentDirectory().createDirectory(); result.failed()) return result;
        juce::TemporaryFile temporary(file);
        {
            juce::FileOutputStream stream(temporary.getFile());
            if (! stream.openedOk()) return stream.getStatus();
            const auto text = juce::JSON::toString(juce::var(root.release()), false);
            if (! stream.write(text.toRawUTF8(), text.getNumBytesAsUTF8())) return juce::Result::fail("Could not write preset.");
            stream.flush();
            if (stream.getStatus().failed()) return stream.getStatus();
        }
        return temporary.overwriteTargetFileWithTemporary() ? juce::Result::ok()
            : juce::Result::fail("Could not replace the preset file; the previous file was preserved.");
    }
    static juce::ValueTree selectionTree(const Preset& preset)
    {
        juce::ValueTree tree("WkPresetSelection");
        tree.setProperty("id", preset.id, nullptr); tree.setProperty("name", preset.name, nullptr);
        tree.setProperty("category", preset.category, nullptr); tree.setProperty("description", preset.description, nullptr);
        for (const auto& [id, value] : preset.values)
        {
            juce::ValueTree child("VALUE"); child.setProperty("id", id, nullptr);
            child.setProperty("value", value, nullptr); tree.addChild(child, -1, nullptr);
        }
        return tree;
    }
    void select(const Preset& preset)
    {
        { const juce::ScopedLock lock(selectionLock); selected = preset; }
        changed();
    }
    void changed() { processor.updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true)); }

    juce::AudioProcessor& processor;
    juce::AudioProcessorValueTreeState& parameters;
    juce::String product;
    const std::vector<Preset>& factory;
    juce::File directory;
    juce::InterProcessLock fileLock;
    mutable juce::CriticalSection selectionLock;
    Preset selected;
};
} // namespace wk
