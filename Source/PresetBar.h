#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PresetLibrary.h"
#include <set>

namespace wk
{
class PresetBar final : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar(PresetLibrary& library) : presets(library)
    {
        setName("Preset library");
        for (auto* button : { &previous, &next, &browse, &favourite, &save, &saveAs, &manage })
        {
            addAndMakeVisible(button);
            button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff18222c));
            button->setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe9edf1));
        }
        previous.setTitle("Previous preset"); next.setTitle("Next preset");
        browse.setTitle("Browse presets"); favourite.setTitle("Toggle favourite");
        save.setTitle("Save preset"); saveAs.setTitle("Save preset as"); manage.setTitle("Manage presets");
        previous.onClick = [this] { step(-1); }; next.onClick = [this] { step(1); };
        browse.onClick = [this] { showBrowser(); };
        favourite.onClick = [this] {
            const auto id = presets.current().id;
            report(presets.setFavourite(id, ! presets.isFavourite(id))); timerCallback();
        };
        save.onClick = [this] { if (presets.current().factoryIndex >= 0) nameDialog(false); else report(presets.save()); };
        saveAs.onClick = [this] { nameDialog(false); };
        manage.onClick = [this] { showManagement(); };
        refreshEntries(); timerCallback(); startTimerHz(5);
    }
    void resized() override
    {
        auto row = getLocalBounds().reduced(0, 2);
        previous.setBounds(row.removeFromLeft(29).reduced(1));
        next.setBounds(row.removeFromLeft(29).reduced(1));
        manage.setBounds(row.removeFromRight(58).reduced(2));
        saveAs.setBounds(row.removeFromRight(74).reduced(2));
        save.setBounds(row.removeFromRight(54).reduced(2));
        favourite.setBounds(row.removeFromRight(37).reduced(2));
        browse.setBounds(row.reduced(3, 1));
    }

private:
    class Browser final : public juce::Component, private juce::ListBoxModel
    {
    public:
        explicit Browser(PresetBar& owner) : bar(&owner)
        {
            setSize(560, 385);
            search.setTextToShowWhenEmpty("Search presets or descriptions", juce::Colour(0xff8090a0));
            search.setTitle("Search presets");
            reset.setButtonText("Clear"); reset.setTitle("Reset preset filters");
            reset.setTooltip("Show all presets and clear the search and category filters.");
            source.addItem("All presets", 1); source.addItem("Factory", 2);
            source.addItem("User", 3); source.addItem("Favourites", 4); source.setSelectedId(owner.sourceFilter, juce::dontSendNotification);
            source.setTitle("Preset source"); category.setTitle("Preset category");
            category.addItem("All categories", 1);
            juce::StringArray categories;
            for (const auto& item : owner.entries) categories.addIfNotAlreadyThere(item.category);
            categories.sort(true);
            for (const auto& item : categories) category.addItem(item, category.getNumItems() + 1);
            category.setSelectedId(1, juce::dontSendNotification);
            if (owner.categoryFilter.isNotEmpty()) category.setText(owner.categoryFilter, juce::dontSendNotification);
            search.setText(owner.searchFilter, false);
            list.setModel(this); list.setRowHeight(34); list.setTitle("Presets");
            list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff101820));
            list.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff334451));
            description.setColour(juce::Label::textColourId, juce::Colour(0xffb3c2d0));
            description.setJustificationType(juce::Justification::topLeft);
            count.setTitle("Preset results"); count.setColour(juce::Label::textColourId, juce::Colour(0xff96abba));
            for (const auto& item : owner.entries)
                if (owner.presets.isFavourite(item.id)) favourites.insert(item.id);
            loadButton.setButtonText("Load preset"); loadButton.setTitle("Load selected preset");
            addAndMakeVisible(search); addAndMakeVisible(reset); addAndMakeVisible(source); addAndMakeVisible(category); addAndMakeVisible(count);
            addAndMakeVisible(list); addAndMakeVisible(description); addAndMakeVisible(loadButton);
            search.onTextChange = [this] { filter(); }; source.onChange = [this] { filter(); };
            category.onChange = [this] { filter(); };
            loadButton.onClick = [this] { loadSelected(); };
            reset.onClick = [this] {
                search.setText({}, false);
                source.setSelectedId(1, juce::dontSendNotification);
                category.setSelectedId(1, juce::dontSendNotification);
                filter();
            };
            filter();
        }
        ~Browser() override { list.setModel(nullptr); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff101820)); }
        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            auto searchRow = area.removeFromTop(29);
            reset.setBounds(searchRow.removeFromRight(58)); searchRow.removeFromRight(6);
            search.setBounds(searchRow); area.removeFromTop(6);
            auto filters = area.removeFromTop(28); source.setBounds(filters.removeFromLeft(170));
            filters.removeFromLeft(8); category.setBounds(filters);
            area.removeFromTop(7);
            count.setBounds(area.removeFromTop(20));
            auto footer = area.removeFromBottom(62);
            loadButton.setBounds(footer.removeFromRight(108).withSizeKeepingCentre(106, 30));
            description.setBounds(footer.reduced(0, 3));
            list.setBounds(area);
        }
    private:
        int getNumRows() override { return static_cast<int>(visible.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selectedRow) override
        {
            if (! juce::isPositiveAndBelow(row, getNumRows())) return;
            const auto& item = visible[static_cast<size_t>(row)];
            if (selectedRow) g.fillAll(juce::Colour(0xff294456));
            g.setColour(juce::Colour(0xffedf2f6)); g.setFont(juce::FontOptions(14.0f));
            if (favourites.count(item.id) != 0)
            {
                g.setColour(juce::Colour(0xffffce74));
                g.drawText(juce::String::fromUTF8("★"), 7, 0, 19, height, juce::Justification::centredLeft);
                g.setColour(juce::Colour(0xffedf2f6));
            }
            g.drawText(item.name, 29, 0, width - 215, height, juce::Justification::centredLeft);
            g.setColour(juce::Colour(0xff96abba)); g.setFont(juce::FontOptions(11.0f));
            g.drawText((item.factoryIndex >= 0 ? "Factory / " : "User / ") + item.category,
                       width - 185, 0, 176, height, juce::Justification::centredRight);
        }
        void selectedRowsChanged(int row) override
        {
            const auto valid = juce::isPositiveAndBelow(row, getNumRows());
            loadButton.setEnabled(valid);
            description.setText(valid ? visible[static_cast<size_t>(row)].description : "No presets match these filters.", juce::dontSendNotification);
        }
        void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { loadSelected(); }
        void returnKeyPressed(int) override { loadSelected(); }
        void filter()
        {
            if (bar == nullptr) return;
            bar->sourceFilter = source.getSelectedId(); bar->searchFilter = search.getText();
            bar->categoryFilter = category.getSelectedId() == 1 ? juce::String() : category.getText();
            visible = bar->filteredEntries(); list.updateContent();
            count.setText(juce::String(static_cast<int>(visible.size())) + " of "
                + juce::String(static_cast<int>(bar->entries.size())) + " presets", juce::dontSendNotification);
            reset.setEnabled(bar->sourceFilter != 1 || bar->searchFilter.isNotEmpty() || bar->categoryFilter.isNotEmpty());
            const auto id = bar->presets.current().id;
            int row = visible.empty() ? -1 : 0;
            for (size_t index = 0; index < visible.size(); ++index)
                if (visible[index].id == id) row = static_cast<int>(index);
            list.selectRow(row); selectedRowsChanged(row); list.repaint();
        }
        void loadSelected()
        {
            const auto row = list.getSelectedRow();
            if (bar == nullptr || ! juce::isPositiveAndBelow(row, getNumRows())) return;
            const auto selected = visible[static_cast<size_t>(row)];
            auto owner = bar;
            if (auto* callout = findParentComponentOfClass<juce::CallOutBox>()) callout->dismiss();
            owner->requestLoad(selected);
        }
        juce::Component::SafePointer<PresetBar> bar;
        juce::TextEditor search;
        juce::ComboBox source, category;
        juce::ListBox list;
        juce::Label description, count;
        juce::TextButton loadButton, reset;
        std::set<juce::String> favourites;
        std::vector<Preset> visible;
    };

    void refreshEntries()
    {
        entries = presets.factoryPresets();
        const auto users = presets.userPresets(); entries.insert(entries.end(), users.begin(), users.end());
    }
    std::vector<Preset> filteredEntries() const
    {
        std::vector<Preset> result;
        for (const auto& item : entries)
        {
            if (sourceFilter == 2 && item.factoryIndex < 0) continue;
            if (sourceFilter == 3 && item.factoryIndex >= 0) continue;
            if (sourceFilter == 4 && ! presets.isFavourite(item.id)) continue;
            if (categoryFilter.isNotEmpty() && item.category != categoryFilter) continue;
            if (searchFilter.isNotEmpty() && ! PresetLibrary::searchKey(item.name + " " + item.category + " " + item.description).contains(PresetLibrary::searchKey(searchFilter))) continue;
            result.push_back(item);
        }
        return result;
    }
    void showBrowser()
    {
        refreshEntries();
        juce::CallOutBox::launchAsynchronously(std::make_unique<Browser>(*this), browse.getScreenBounds(), nullptr);
    }
    void step(int direction)
    {
        refreshEntries(); const auto visible = filteredEntries();
        if (visible.empty()) return;
        const auto id = presets.current().id;
        int index = direction > 0 ? -1 : 0;
        for (size_t i = 0; i < visible.size(); ++i) if (visible[i].id == id) index = static_cast<int>(i);
        index = (index + direction + static_cast<int>(visible.size())) % static_cast<int>(visible.size());
        requestLoad(visible[static_cast<size_t>(index)]);
    }
    void requestLoad(Preset preset)
    {
        const juce::Component::SafePointer<PresetBar> safe(this);
        if (! presets.isModified()) { report(presets.load(preset)); return; }
        const auto originalSound = presets.currentSound();
        juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Unsaved preset changes").withMessage("Save your current sound before loading " + preset.name + "?")
            .withButton("Save As...").withButton("Discard changes").withButton("Cancel").withAssociatedComponent(this),
            [safe, preset, originalSound](int result) {
                if (safe == nullptr || (result != 1 && result != 2)) return;
                if (! safe->checkSound(originalSound)) return;
                if (result == 1) safe->nameDialog(false, [safe, preset] { if (safe != nullptr) safe->report(safe->presets.load(preset)); });
                else if (result == 2) safe->report(safe->presets.load(preset));
            });
    }
    void nameDialog(bool rename, std::function<void()> afterSave = {})
    {
        const auto current = presets.currentSound();
        auto* dialog = new juce::AlertWindow(rename ? "Rename preset" : "Save preset as",
            rename ? "Rename the saved user preset." : "Store all current sound settings as your own preset.", juce::MessageBoxIconType::NoIcon, this);
        dialog->addTextEditor("name", rename ? current.name : current.name.substring(0, 75) + " Copy", "Name");
        if (! rename) dialog->addTextEditor("category", current.category, "Category");
        dialog->addButton(rename ? "Rename" : "Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        const juce::Component::SafePointer<PresetBar> safe(this);
        dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe, dialog, rename, afterSave, current](int result) {
            if (safe == nullptr || result != 1) return;
            // Save the sound frozen when the dialog opened: the user named what they
            // saw, so host automation during the dialog neither cancels the save nor
            // changes the stored values. Rename stays guarded by the preset identity.
            const auto status = rename ? safe->presets.renameCurrent(dialog->getTextEditorContents("name"), current.id)
                : safe->presets.saveAs(current, dialog->getTextEditorContents("name"), dialog->getTextEditorContents("category"));
            safe->report(status);
            if (status.wasOk() && afterSave) afterSave();
        }), true);
    }
    void showManagement()
    {
        const auto current = presets.current();
        juce::PopupMenu menu;
        menu.addItem(1, "Rename...", current.factoryIndex < 0);
        menu.addItem(2, "Delete...", current.factoryIndex < 0);
        menu.addSeparator(); menu.addItem(3, "Import preset..."); menu.addItem(4, "Export current sound...");
        const juce::Component::SafePointer<PresetBar> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&manage), [safe, current](int result) {
            if (safe == nullptr) return;
            if ((result == 1 || result == 2) && safe->presets.current().id != current.id)
            {
                safe->report(juce::Result::fail("The selected preset changed while the menu was open. Please choose the action again."));
                return;
            }
            if (result == 1) safe->nameDialog(true);
            if (result == 2)
                juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("Delete user preset?").withMessage("Delete " + current.name + " from the library? The current sound remains in this project.")
                    .withButton("Delete").withButton("Cancel").withAssociatedComponent(safe.getComponent()),
                    [safe, current](int choice) { if (safe != nullptr && choice == 1 && safe->presets.current().id == current.id) safe->report(safe->presets.deleteCurrent()); });
            if (result == 3 || result == 4) safe->chooseFile(result == 3);
        });
    }
    void chooseFile(bool importing)
    {
        const auto originalSound = presets.currentSound();
        chooser = std::make_unique<juce::FileChooser>(importing ? "Import preset" : "Export current sound",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(juce::File::createLegalFileName(presets.current().name) + presets.extension()), "*" + presets.extension());
        const juce::Component::SafePointer<PresetBar> safe(this);
        const auto flags = importing ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting;
        chooser->launchAsync(flags, [safe, importing, originalSound](const juce::FileChooser& completed) {
            if (safe == nullptr || completed.getResult() == juce::File()) return;
            if (importing)
            {
                Preset imported; const auto status = safe->presets.importPreset(completed.getResult(), imported);
                safe->report(status); if (status.wasOk()) safe->requestLoad(imported);
            }
            else safe->report(safe->presets.exportPreset(originalSound, completed.getResult()));
        });
    }
    bool checkSound(const Preset& expected)
    {
        if (presets.isCurrentSound(expected)) return true;
        report(juce::Result::fail("The sound changed while the dialog was open. The action was cancelled to preserve the current sound. Please try again."));
        return false;
    }
    void report(const juce::Result& result)
    {
        if (result.failed()) juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon).withTitle("Preset library")
            .withMessage(result.getErrorMessage()).withButton("OK").withAssociatedComponent(this),
            [](int) {}); // A null callback can enter a nested modal loop in JUCE.
        refreshEntries(); timerCallback();
    }
    void timerCallback() override
    {
        const auto preset = presets.current();
        browse.setButtonText((preset.factoryIndex >= 0 ? "Factory / " : "User / ") + preset.name + (presets.isModified() ? " *" : "") + juce::String::fromUTF8("  ▾"));
        browse.setTooltip(preset.category + ": " + preset.description + "\n* indicates unsaved changes.");
        // File reads are limited to editor/UI timers, never the audio callback.
        favourite.setButtonText(presets.isFavourite(preset.id) ? juce::String::fromUTF8("★") : juce::String::fromUTF8("☆"));
    }
    PresetLibrary& presets;
    juce::TextButton previous { "<" }, next { ">" }, browse, favourite, save { "Save" }, saveAs { "Save As" }, manage { "More" };
    std::vector<Preset> entries;
    int sourceFilter = 1;
    juce::String searchFilter, categoryFilter;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace wk
