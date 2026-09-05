#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PresetLibrary.h"
#include <cstdint>

namespace wk
{
class PresetBar final : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar(PresetLibrary& libraryToUse) : presets(libraryToUse), ownerWatcher(*this)
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
            if (! canOpenUi()) return;
            const juce::Component::SafePointer<PresetBar> safe(this);
            const auto generation = uiGeneration;
            const auto id = presets.current().id;
            const auto enabled = ! presets.isFavourite(id);
            applyAndReport(safe, generation, [id, enabled](PresetLibrary& library) { return library.setFavourite(id, enabled); });
        };
        save.onClick = [this] {
            if (! canOpenUi()) return;
            if (presets.current().factoryIndex >= 0) { nameDialog(false); return; }
            applyAndReport(this, uiGeneration, [](PresetLibrary& library) { return library.save(); });
        };
        saveAs.onClick = [this] { nameDialog(false); };
        manage.onClick = [this] { showManagement(); };
        refreshEntries(); timerCallback(); startTimerHz(5);
    }
    ~PresetBar() override
    {
        // SafePointer<this> is not cleared until Component's base destructor.
        // Invalidate callbacks before closing any window or destroying members.
        destroying = true;
        stopTimer();
        cancelOwnedUi();
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
        if (managementParent != nullptr)
            if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
                managementParent->setBounds(editor->getLocalBounds());
    }

private:
    // Observe ancestors too: a host can hide or detach the editor while leaving
    // this child visible/alive. Component::visibilityChanged alone misses that.
    class OwnerWatcher final : public juce::ComponentMovementWatcher
    {
    public:
        explicit OwnerWatcher(PresetBar& bar) : ComponentMovementWatcher(&bar), owner(bar) {}
        void componentMovedOrResized(bool, bool) override {}
        void componentPeerChanged() override { owner.cancelOwnedUi(); }
        void componentVisibilityChanged() override { if (! owner.isShowing()) owner.cancelOwnedUi(); }
    private:
        PresetBar& owner;
    };

    struct MessageScope
    {
        juce::ScopedMessageBox box;
        bool completed = false;
    };

    bool canOpenUi() const { return ! destroying && ! cancellingUi && isShowing(); }
    bool acceptsCallback(std::uint64_t generation) const { return generation == uiGeneration && canOpenUi(); }

    template <typename Action>
    static bool applyAndReport(juce::Component::SafePointer<PresetBar> owner, std::uint64_t generation, Action action)
    {
        if (owner == nullptr || ! owner->acceptsCallback(generation)) return false;
        // The processor owns the library. A mutation can synchronously notify
        // the host, which may destroy/hide the editor before the call returns.
        // Keep the action/inputs on this stack, not in an editor-owned callback,
        // and never evaluate report(mutation()) on a previously checked owner.
        const auto result = action(owner->presets);
        if (owner == nullptr || ! owner->acceptsCallback(generation)) return false;
        owner->report(result);
        return owner != nullptr && owner->acceptsCallback(generation) && result.wasOk();
    }

    static void cancelModal(juce::Component::SafePointer<juce::Component> component)
    {
        if (component == nullptr) return;
        component->exitModalState(0);
        if (component == nullptr) return;
        component->setVisible(false);
        if (component != nullptr) component->setLookAndFeel(nullptr);
        // JUCE owns these modal windows and deletes them asynchronously. Never
        // pump a nested message loop, or delete CallOutBox's callback-owned data.
    }

    void cancelOwnedUi()
    {
        if (cancellingUi) return;
        const juce::ScopedValueSetter<bool> cancelling(cancellingUi, true);
        ++uiGeneration;
        // Move handles out before closing: cancellation must remain safe if a
        // platform callback runs synchronously while a native window is closed.
        auto modals = std::move(ownedModals);
        ownedModals.clear();
        auto messages = std::move(messageScopes);
        messageScopes.clear();
        managementParent.reset(); // Cancels only the menu parented here.
        chooser.reset();          // FileChooser clears its pending callback.
        for (auto& message : messages) message->box.close();
        for (auto modal : modals) cancelModal(modal);
    }

    void pruneUiHandles()
    {
        std::erase_if(ownedModals, [](const auto& modal) { return modal == nullptr; });
        std::erase_if(messageScopes, [](const auto& scope) { return scope->completed; });
    }

    void showMessage(const juce::MessageBoxOptions& options, std::function<void(int)> callback)
    {
        if (! canOpenUi()) return;
        pruneUiHandles();
        const auto scope = std::make_shared<MessageScope>();
        messageScopes.push_back(scope);
        const std::weak_ptr<MessageScope> weakScope(scope);
        const juce::Component::SafePointer<PresetBar> safe(this);
        const auto generation = uiGeneration;
        scope->box = juce::AlertWindow::showScopedAsync(options,
            [safe, generation, weakScope, callback = std::move(callback)](int result) {
                // Keep the completed scope alive through a callback that may
                // open another message and prune the previous vector entry.
                const auto completed = weakScope.lock();
                if (completed != nullptr) completed->completed = true;
                if (safe != nullptr && safe->acceptsCallback(generation)) callback(result);
            });
    }

    class Browser final : public juce::Component, private juce::ListBoxModel
    {
    public:
        explicit Browser(PresetBar& owner) : bar(&owner), generation(owner.uiGeneration)
        {
            setSize(560, 385);
            search.setTextToShowWhenEmpty("Search presets or descriptions", juce::Colour(0xff8090a0));
            search.setTitle("Search presets");
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
            loadButton.setButtonText("Load preset"); loadButton.setTitle("Load selected preset");
            addAndMakeVisible(search); addAndMakeVisible(source); addAndMakeVisible(category);
            addAndMakeVisible(list); addAndMakeVisible(description); addAndMakeVisible(loadButton);
            search.onTextChange = [this] { filter(); }; source.onChange = [this] { filter(); };
            category.onChange = [this] { filter(); };
            loadButton.onClick = [this] { loadSelected(); };
            filter();
        }
        ~Browser() override { list.setModel(nullptr); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff101820)); }
        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            search.setBounds(area.removeFromTop(29)); area.removeFromTop(6);
            auto filters = area.removeFromTop(28); source.setBounds(filters.removeFromLeft(170));
            filters.removeFromLeft(8); category.setBounds(filters);
            area.removeFromTop(7);
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
            g.drawText(item.name, 9, 0, width - 190, height, juce::Justification::centredLeft);
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
            if (bar == nullptr || ! bar->acceptsCallback(generation)) return;
            bar->sourceFilter = source.getSelectedId(); bar->searchFilter = search.getText();
            bar->categoryFilter = category.getSelectedId() == 1 ? juce::String() : category.getText();
            visible = bar->filteredEntries(); list.updateContent();
            const auto id = bar->presets.current().id;
            int row = visible.empty() ? -1 : 0;
            for (size_t index = 0; index < visible.size(); ++index)
                if (visible[index].id == id) row = static_cast<int>(index);
            list.selectRow(row); selectedRowsChanged(row); list.repaint();
        }
        void loadSelected()
        {
            const auto row = list.getSelectedRow();
            if (bar == nullptr || ! bar->acceptsCallback(generation) || ! juce::isPositiveAndBelow(row, getNumRows())) return;
            const auto selected = visible[static_cast<size_t>(row)];
            auto owner = bar;
            if (auto* callout = findParentComponentOfClass<juce::CallOutBox>()) callout->dismiss();
            owner->requestLoad(selected);
        }
        juce::Component::SafePointer<PresetBar> bar;
        const std::uint64_t generation;
        juce::TextEditor search;
        juce::ComboBox source, category;
        juce::ListBox list;
        juce::Label description;
        juce::TextButton loadButton;
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
        if (! canOpenUi()) return;
        auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (editor == nullptr) return;
        refreshEntries();
        pruneUiHandles();
        const juce::Component::SafePointer<PresetBar> safe(this);
        const auto generation = uiGeneration;
        auto& callout = juce::CallOutBox::launchAsynchronously(std::make_unique<Browser>(*this),
            editor->getLocalArea(&browse, browse.getLocalBounds()), editor);
        if (safe != nullptr && safe->acceptsCallback(generation)) safe->ownedModals.emplace_back(&callout);
        else cancelModal(&callout);
    }
    void step(int direction)
    {
        if (! canOpenUi()) return;
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
        if (! canOpenUi()) return;
        const juce::Component::SafePointer<PresetBar> safe(this);
        const auto generation = uiGeneration;
        const auto originalSound = presets.currentSoundToken();
        if (! originalSound.has_value())
        {
            report(juce::Result::fail("The plugin state is changing. Please try again."));
            return;
        }
        if (! presets.isModified())
        {
            applyAndReport(safe, generation, [preset, originalSound = *originalSound](PresetLibrary& library) {
                return library.load(preset, &originalSound);
            });
            return;
        }
        showMessage(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Unsaved preset changes").withMessage("Save your current sound before loading " + preset.name + "?")
            .withButton("Save As...").withButton("Discard changes").withButton("Cancel").withAssociatedComponent(this),
            [safe, generation, preset, originalSound = *originalSound](int result) {
                if (safe == nullptr || ! safe->acceptsCallback(generation) || (result != 1 && result != 2)) return;
                if (! safe->checkSound(originalSound)) return;
                if (result == 1) safe->nameDialog(false, [safe, generation, preset](const PresetLibrary::SoundToken& savedSound) {
                    applyAndReport(safe, generation, [preset, savedSound](PresetLibrary& library) {
                        return library.load(preset, &savedSound);
                    });
                }, originalSound);
                else if (result == 2)
                    applyAndReport(safe, generation, [preset, originalSound](PresetLibrary& library) {
                        return library.load(preset, &originalSound);
                    });
            });
    }
    void nameDialog(bool rename, std::function<void(const PresetLibrary::SoundToken&)> afterSave = {},
                    std::optional<PresetLibrary::SoundToken> expected = std::nullopt)
    {
        if (! canOpenUi()) return;
        pruneUiHandles();
        const auto generation = uiGeneration;
        if (! expected.has_value()) expected = presets.currentSoundToken();
        if (! expected.has_value())
        {
            report(juce::Result::fail("The plugin state is changing. Please try again."));
            return;
        }
        const auto token = *expected;
        const auto& current = token.sound;
        auto* dialog = new juce::AlertWindow(rename ? "Rename preset" : "Save preset as",
            rename ? "Rename the saved user preset." : "Store all current sound settings as your own preset.", juce::MessageBoxIconType::NoIcon, this);
        dialog->addTextEditor("name", rename ? current.name : current.name.substring(0, 75) + " Copy", "Name");
        if (! rename) dialog->addTextEditor("category", current.category, "Category");
        dialog->addButton(rename ? "Rename" : "Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        const juce::Component::SafePointer<PresetBar> safe(this);
        const juce::Component::SafePointer<juce::AlertWindow> dialogLifetime(dialog);
        ownedModals.emplace_back(dialog);
        dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe, generation, dialogLifetime, rename, afterSave, token](int result) {
            if (safe == nullptr || ! safe->acceptsCallback(generation) || dialogLifetime == nullptr || result != 1) return;
            const auto name = dialogLifetime->getTextEditorContents("name");
            const auto category = rename ? juce::String() : dialogLifetime->getTextEditorContents("category");
            bool savedSelectionStillCurrent = false;
            std::optional<PresetLibrary::SoundToken> savedSound;
            if (applyAndReport(safe, generation, [rename, name, category, token, &savedSelectionStillCurrent, &savedSound](PresetLibrary& library) {
                    return rename ? library.renameCurrent(name, token.sound.id, &token)
                                  : library.saveAs(name, category, &savedSelectionStillCurrent, &token, &savedSound);
                }) && afterSave && savedSelectionStillCurrent)
            {
                if (savedSound.has_value()) afterSave(*savedSound);
                else safe->report(juce::Result::fail("The plugin state is changing. The requested preset was not loaded."));
            }
        }), true);
        if (safe == nullptr || ! safe->acceptsCallback(generation)) cancelModal(dialogLifetime.getComponent());
    }
    void showManagement()
    {
        if (! canOpenUi()) return;
        auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (editor == nullptr) return;
        const auto currentToken = presets.currentSoundToken();
        if (! currentToken.has_value())
        {
            report(juce::Result::fail("The plugin state is changing. Please try again."));
            return;
        }
        // A private, editor-sized parent gives this menu an explicit lifetime.
        // Destroying it cancels only our menu, including on hide/peer changes.
        managementParent = std::make_unique<juce::Component>();
        managementParent->setName("Preset management menu owner");
        managementParent->setBounds(editor->getLocalBounds());
        managementParent->setInterceptsMouseClicks(false, true);
        editor->addAndMakeVisible(*managementParent);
        const juce::Component::SafePointer<juce::Component> menuOwner(managementParent.get());
        const auto generation = uiGeneration;
        const auto current = currentToken->sound;
        juce::PopupMenu menu;
        menu.addItem(1, "Rename...", current.factoryIndex < 0);
        menu.addItem(2, "Delete...", current.factoryIndex < 0);
        menu.addSeparator(); menu.addItem(3, "Import preset..."); menu.addItem(4, "Export current sound...");
        const juce::Component::SafePointer<PresetBar> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&manage)
            .withParentComponent(menuOwner.getComponent()).withDeletionCheck(*this), [safe, generation, menuOwner, currentToken = *currentToken](int result) {
            if (safe == nullptr || ! safe->acceptsCallback(generation) || menuOwner == nullptr
                || safe->managementParent.get() != menuOwner.getComponent()) return;
            safe->managementParent.reset();
            if ((result == 1 || result == 2) && ! safe->checkSound(currentToken))
            {
                return;
            }
            if (result == 1) safe->nameDialog(true, {}, currentToken);
            if (result == 2)
                safe->showMessage(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("Delete user preset?").withMessage("Delete " + currentToken.sound.name + " from the library? The current sound remains in this project.")
                    .withButton("Delete").withButton("Cancel").withAssociatedComponent(safe.getComponent()),
                    [safe, generation, currentToken](int choice) {
                        if (safe != nullptr && safe->acceptsCallback(generation) && choice == 1)
                            applyAndReport(safe, generation, [currentToken](PresetLibrary& library) {
                                return library.deleteCurrent(&currentToken);
                            });
                    });
            if (result == 3 || result == 4) safe->chooseFile(result == 3);
        });
    }
    void chooseFile(bool importing)
    {
        if (! canOpenUi()) return;
        const auto generation = uiGeneration;
        const auto exportToken = importing ? std::optional<PresetLibrary::SoundToken> {} : presets.currentSoundToken();
        if (! importing && ! exportToken.has_value())
        {
            report(juce::Result::fail("The plugin state is changing. Please try again."));
            return;
        }
        const auto currentName = importing ? presets.current().name : exportToken->sound.name;
        chooser = std::make_unique<juce::FileChooser>(importing ? "Import preset" : "Export current sound",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(juce::File::createLegalFileName(currentName) + presets.extension()), "*" + presets.extension());
        const juce::Component::SafePointer<PresetBar> safe(this);
        const auto browserFlags = importing ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting;
        chooser->launchAsync(browserFlags, [safe, generation, importing, exportToken](const juce::FileChooser& completed) {
            if (safe == nullptr || ! safe->acceptsCallback(generation) || safe->chooser.get() != &completed
                || completed.getResult() == juce::File()) return;
            const auto file = completed.getResult();
            if (importing)
            {
                Preset imported;
                if (applyAndReport(safe, generation, [file, &imported](PresetLibrary& library) { return library.importPreset(file, imported); }))
                    safe->requestLoad(imported);
            }
            else
                applyAndReport(safe, generation, [file, exportToken](PresetLibrary& library) {
                    return library.exportCurrent(file, &*exportToken);
                });
        });
    }
    bool checkSound(const PresetLibrary::SoundToken& expected)
    {
        if (presets.isCurrentSound(expected)) return true;
        report(juce::Result::fail("The sound changed while the dialog was open. The action was cancelled to preserve the current sound. Please try again."));
        return false;
    }
    void report(const juce::Result& result)
    {
        if (destroying || cancellingUi) return;
        if (result.failed()) showMessage(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon).withTitle("Preset library")
            .withMessage(result.getErrorMessage()).withButton("OK").withAssociatedComponent(this),
            [](int) {}); // A null callback can enter a nested modal loop in JUCE.
        refreshEntries(); timerCallback();
    }
    void timerCallback() override
    {
        pruneUiHandles();
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
    std::uint64_t uiGeneration = 0;
    bool destroying = false, cancellingUi = false;
    std::vector<juce::Component::SafePointer<juce::Component>> ownedModals;
    std::vector<std::shared_ptr<MessageScope>> messageScopes;
    std::unique_ptr<juce::Component> managementParent;
    std::unique_ptr<juce::FileChooser> chooser;
    OwnerWatcher ownerWatcher;
};
} // namespace wk
