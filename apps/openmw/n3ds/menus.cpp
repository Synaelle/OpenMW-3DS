#include "menus.hpp"

#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/journal.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/bookwindow.hpp"
#include "../mwgui/container.hpp"
#include "../mwgui/dialogue.hpp"
#include "../mwgui/inventorywindow.hpp"
#include "../mwgui/itemmodel.hpp"
#include "../mwgui/levelupdialog.hpp"
#include "../mwgui/merchantrepair.hpp"
#include "../mwgui/messagebox.hpp"
#include "../mwgui/scrollwindow.hpp"
#include "../mwgui/sortfilteritemmodel.hpp"
#include "../mwgui/spellbuyingwindow.hpp"
#include "../mwgui/spellmodel.hpp"
#include "../mwgui/spellwindow.hpp"
#include "../mwgui/tradeitemmodel.hpp"
#include "../mwgui/tradewindow.hpp"
#include "../mwgui/trainingwindow.hpp"
#include "../mwgui/travelwindow.hpp"
#include "../mwgui/waitdialog.hpp"
#include "../mwgui/windowmanagerimp.hpp"
#include "../mwdialogue/journalentry.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwstate/character.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"

#include <components/esm3/loadbook.hpp>
#include <components/widgets/box.hpp>
#include <components/widgets/imagebutton.hpp>
#include <components/widgets/list.hpp>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_TextBox.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace omw3ds
{
    namespace
    {
        // MyGUI captions: "#RRGGBB" colour codes out, "##" is a '#'.
        std::string plainText(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '#' && i + 1 < text.size() && text[i + 1] == '#')
                {
                    out += '#';
                    ++i;
                    continue;
                }
                if (text[i] == '#' && i + 6 < text.size())
                {
                    bool hex = true;
                    for (size_t k = 1; hex && k <= 6; ++k)
                        hex = std::isxdigit(static_cast<unsigned char>(text[i + k])) != 0;
                    if (hex)
                    {
                        i += 6;
                        continue;
                    }
                }
                out += text[i];
            }
            return out;
        }

        std::string caption(const MyGUI::TextBox* widget)
        {
            return widget ? plainText(widget->getCaption().asUTF8()) : std::string();
        }

        // Book and scroll text: Morrowind's HTML subset to plain paragraphs.
        std::string bookText(std::string_view html)
        {
            std::string out;
            for (size_t i = 0; i < html.size(); ++i)
            {
                if (html[i] == '<')
                {
                    const size_t end = html.find('>', i);
                    if (end == std::string_view::npos)
                        break;
                    std::string tag(html.substr(i + 1, end - i - 1));
                    for (char& c : tag)
                        c = char(std::toupper(static_cast<unsigned char>(c)));
                    if (tag.rfind("BR", 0) == 0)
                        out += '\n';
                    else if (tag.rfind("P", 0) == 0 || tag.rfind("/P", 0) == 0 || tag.rfind("DIV", 0) == 0)
                    {
                        if (!out.empty() && out.back() != '\n')
                            out += '\n';
                    }
                    i = end;
                    continue;
                }
                if (html[i] == '\r')
                    continue;
                out += html[i];
            }
            std::string tidy;
            int newlines = 0;
            for (char c : out)
            {
                if (c == '\n')
                {
                    if (tidy.empty() || ++newlines > 2)
                        continue;
                }
                else
                    newlines = 0;
                tidy += c;
            }
            while (!tidy.empty() && (tidy.back() == '\n' || tidy.back() == ' '))
                tidy.pop_back();
            return tidy;
        }

        std::string itemName(const MWWorld::Ptr& item)
        {
            return std::string(item.getClass().getName(item));
        }

        // "x3  equipped  value 15"
        std::string stackDetail(const MWGui::ItemStack& stack)
        {
            std::string detail;
            if (stack.mCount > 1)
                detail += "x" + std::to_string(stack.mCount) + "  ";
            if (stack.mType == MWGui::ItemStack::Type_Equipped)
                detail += "equipped  ";
            else if (stack.mType == MWGui::ItemStack::Type_Barter)
                detail += "in trade  ";
            detail += "value " + std::to_string(stack.mBase.getClass().getValue(stack.mBase));
            return detail;
        }

        // Sub-pages of a menu (the item chosen in the inventory, the buy or sell list of a barter...).
        enum class Page
        {
            Main,
            Item,
            Magic,
            Buy,
            Sell,
            Load,
        };

        struct State
        {
            MWGui::GuiMode mMode = MWGui::GM_None;
            Page mPage = Page::Main;
            MWWorld::Ptr mItem;
        } sState;

        int sPressed = -1; // pressMenuOption
    }

    // Friend of the MWGui windows it reads and drives (OMW3DS_MENU_FRIEND).
    struct MenuBridge
    {
        p3ds::MenuView mView;
        std::vector<std::function<void()>> mActions;

        static MWGui::WindowManager& windows()
        {
            return static_cast<MWGui::WindowManager&>(*MWBase::Environment::get().getWindowManager());
        }

        template <class T>
        static T* window(std::string_view luaId)
        {
            MWGui::WindowManager& wm = windows();
            const auto it = wm.mLuaIdToWindow.find(luaId);
            return it == wm.mLuaIdToWindow.end() ? nullptr : dynamic_cast<T*>(it->second);
        }

        void option(std::string label, std::function<void()> action, std::string detail = {})
        {
            mView.mOptions.push_back({ std::move(label), std::move(detail) });
            mActions.push_back(std::move(action));
        }

        // A button of an OpenMW window, as an option: its caption, and its click handler.
        template <class W, class B>
        void button(B* widget, W* target, void (W::*handler)(MyGUI::Widget*), std::string detail = {})
        {
            if (widget == nullptr || !widget->getVisible())
                return;
            if (!widget->getEnabled())
                detail = detail.empty() ? "unavailable" : detail + "  unavailable";
            option(
                caption(widget),
                [widget, target, handler] {
                    if (widget->getEnabled())
                        (target->*handler)(widget);
                },
                std::move(detail));
        }

        static void setPage(Page page, MWWorld::Ptr item = {})
        {
            sState.mPage = page;
            sState.mItem = item;
        }

        // ------------------------------------------------------------ message boxes

        // A message box with buttons (scripts' choices, confirmations): it holds the game until answered.
        bool messageBox()
        {
            MWGui::MessageBoxManager* manager = windows().mMessageBoxManager.get();
            MWGui::InteractiveMessageBox* box = manager ? manager->mInterMessageBoxe.get() : nullptr;
            if (box == nullptr || box->mMarkedToDelete)
                return false;
            mView.mTitle = "MESSAGE";
            mView.mBody.push_back(caption(box->mMessageWidget));
            for (MyGUI::Button* b : box->mButtons)
                option(caption(b), [box, b] { box->buttonActivated(b); });
            return true;
        }

        // ------------------------------------------------------------ dialogue

        // The dialogue's Persuasion choice: a modal window over it.
        void persuasion(MWGui::PersuasionDialog& dialog, const MWWorld::Ptr& actor)
        {
            mView.mTitle = "Persuade " + itemName(actor);
            mView.mBody.push_back(caption(dialog.mGoldLabel));
            for (MyGUI::Button* b : { dialog.mAdmireButton, dialog.mIntimidateButton, dialog.mTauntButton,
                     dialog.mBribe10Button, dialog.mBribe100Button, dialog.mBribe1000Button })
                if (b->getEnabled())
                    option(caption(b), [&dialog, b] { dialog.onPersuade(b); });
            option("Cancel", [&dialog] { dialog.onCancel(nullptr); });
        }

        bool dialogue()
        {
            MWGui::DialogueWindow* window = windows().mDialogueWindow;
            if (window == nullptr || window->mPtr.isEmpty())
                return false;
            const MWWorld::Ptr actor = window->mPtr;
            if (window->mPersuasionDialog.isVisible())
            {
                persuasion(window->mPersuasionDialog, actor);
                return true;
            }
            mView.mTitle = itemName(actor);
            if (actor.getClass().isNpc())
                mView.mTitle += "  (disposition "
                    + std::to_string(MWBase::Environment::get().getMechanicsManager()->getDerivedDisposition(actor))
                    + ")";
            mView.mBodyFromEnd = true;
            for (const std::unique_ptr<MWGui::DialogueText>& entry : window->mHistoryContents)
            {
                if (!mView.mBody.empty())
                    mView.mBody.emplace_back();
                if (const auto* response = dynamic_cast<const MWGui::Response*>(entry.get()))
                {
                    if (!response->mTitle.empty())
                        mView.mBody.push_back("[" + response->mTitle + "]");
                    mView.mBody.push_back(plainText(response->mText));
                }
                else
                    mView.mBody.push_back("(" + plainText(entry->mText) + ")");
            }

            MWBase::DialogueManager* dialogueManager = MWBase::Environment::get().getDialogueManager();
            if (window->mGoodbye)
                option("Goodbye", [window] { window->onGoodbyeActivated(); });
            else if (dialogueManager->isInChoice())
            {
                for (const auto& [text, id] : dialogueManager->getChoices())
                    option(plainText(text), [window, id = id] { window->onChoiceActivated(id); });
            }
            else
            {
                // Services (Barter, Travel, ...) come first, then a separator ("") and the topics.
                for (size_t i = 0; i < window->mTopicsList->getItemCount(); ++i)
                {
                    const std::string name = window->mTopicsList->getItemNameAt(i);
                    if (!name.empty())
                        option(name, [window, name] { window->onSelectListItem(name, 0); });
                }
                option("Goodbye", [window] { window->onByeClicked(nullptr); });
            }
            return true;
        }

        // ------------------------------------------------------------ inventory and magic

        static std::string playerSummary()
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const int gold = player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId);
            const float weight = player.getClass().getEncumbrance(player);
            const float capacity = player.getClass().getCapacity(player);
            return "Gold " + std::to_string(gold) + "   weight " + std::to_string(int(weight)) + " / "
                + std::to_string(int(capacity));
        }

        // Index of the stack holding `item` in a sorted model, or -1.
        static int findStack(MWGui::SortFilterItemModel& model, const MWWorld::Ptr& item)
        {
            for (size_t i = 0; i < model.getItemCount(); ++i)
                if (model.getItem(i).mBase == item)
                    return int(i);
            return -1;
        }

        void magic()
        {
            MWGui::SpellWindow* spells = windows().mSpellWindow;
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            MWGui::SpellModel model(player);
            model.update();
            mView.mTitle = "MAGIC";
            mView.mBody.push_back("Choose the spell or enchanted item Y readies.");
            option("Back", [] { setPage(Page::Main); });
            for (size_t i = 0; i < model.getItemCount(); ++i)
            {
                const MWGui::Spell spell = model.getItem(int(i));
                std::string detail = plainText(spell.mCostColumn);
                if (spell.mType == MWGui::Spell::Type_Power)
                    detail = "power";
                if (spell.mSelected)
                    detail += "  selected";
                if (spell.mType == MWGui::Spell::Type_EnchantedItem)
                    option(
                        spell.mName,
                        [spells, item = spell.mItem, active = spell.mActive] {
                            spells->onEnchantedItemSelected(item, active);
                        },
                        detail);
                else
                    option(spell.mName, [spells, id = spell.mId] { spells->onSpellSelected(id); }, detail);
            }
        }

        void inventory()
        {
            MWGui::InventoryWindow* inv = windows().mInventoryWindow;
            MWGui::SortFilterItemModel& model = *inv->mSortModel;
            if (sState.mPage == Page::Magic)
            {
                magic();
                return;
            }
            const int itemIndex = sState.mPage == Page::Item ? findStack(model, sState.mItem) : -1;
            if (itemIndex < 0)
                setPage(Page::Main);
            if (sState.mPage == Page::Item)
            {
                const MWGui::ItemStack stack = model.getItem(itemIndex);
                const size_t source = model.mapToSource(itemIndex);
                mView.mTitle = itemName(stack.mBase);
                mView.mBody.push_back(stackDetail(stack));
                char weight[32];
                std::snprintf(weight, sizeof(weight), "weight %.1f", double(stack.mBase.getClass().getWeight(stack.mBase)));
                mView.mBody.push_back(weight);
                const bool equipped = stack.mType == MWGui::ItemStack::Type_Equipped;
                option(equipped ? "Unequip" : "Use / equip", [inv, source] {
                    inv->mSelectedItem = int(source);
                    inv->equipItem(1);
                    setPage(Page::Main);
                });
                option("Drop", [inv, source] {
                    inv->mSelectedItem = int(source);
                    inv->dropItem(nullptr, 1);
                    setPage(Page::Main);
                });
                if (stack.mCount > 1)
                    option(
                        "Drop all",
                        [inv, source, count = stack.mCount] {
                            inv->mSelectedItem = int(source);
                            inv->dropItem(nullptr, count);
                            setPage(Page::Main);
                        },
                        "x" + std::to_string(stack.mCount));
                option("Back", [] { setPage(Page::Main); });
                return;
            }
            mView.mTitle = "INVENTORY";
            mView.mBody.push_back(playerSummary());
            mView.mBody.push_back("B closes.");
            option("[Magic]", [] { setPage(Page::Magic); }, "spells, enchanted items");
            for (size_t i = 0; i < model.getItemCount(); ++i)
            {
                const MWGui::ItemStack stack = model.getItem(i);
                option(itemName(stack.mBase), [item = stack.mBase] { setPage(Page::Item, item); }, stackDetail(stack));
            }
        }

        // ------------------------------------------------------------ containers

        bool container()
        {
            MWGui::ContainerWindow* cw = windows().mContainerWindow;
            if (cw == nullptr || cw->mPtr.isEmpty() || cw->mModel == nullptr)
                return false;
            mView.mTitle = itemName(cw->mPtr);
            if (mView.mTitle.empty())
                mView.mTitle = "CONTAINER";
            mView.mBody.push_back(playerSummary());
            button(cw->mTakeButton, cw, &MWGui::ContainerWindow::onTakeAllButtonClicked);
            button(cw->mDisposeCorpseButton, cw, &MWGui::ContainerWindow::onDisposeCorpseButtonClicked);
            button(cw->mCloseButton, cw, &MWGui::ContainerWindow::onCloseButtonClicked);
            MWGui::SortFilterItemModel& model = *cw->mSortModel;
            for (size_t i = 0; i < model.getItemCount(); ++i)
            {
                const MWGui::ItemStack stack = model.getItem(i);
                option(
                    itemName(stack.mBase),
                    [cw, source = model.mapToSource(i), count = stack.mCount] {
                        cw->mSelectedItem = int(source);
                        cw->transferItem(nullptr, count);
                    },
                    "take  " + stackDetail(stack));
            }
            return true;
        }

        // ------------------------------------------------------------ barter

        bool barter()
        {
            MWGui::TradeWindow* trade = windows().mTradeWindow;
            MWGui::InventoryWindow* inv = windows().mInventoryWindow;
            if (trade == nullptr || trade->mPtr.isEmpty())
                return false;
            mView.mTitle = "BARTER: " + itemName(trade->mPtr);
            mView.mBody.push_back(caption(trade->mPlayerGold) + "   " + caption(trade->mMerchantGold));
            mView.mBody.push_back(caption(trade->mTotalBalanceLabel) + " " + std::to_string(trade->mCurrentBalance));
            if (sState.mPage == Page::Buy || sState.mPage == Page::Sell)
            {
                const bool buy = sState.mPage == Page::Buy;
                mView.mBody.push_back(buy ? "Touch an item to add it to the deal (again: take it back)."
                                          : "Touch an item to offer it (again: take it back).");
                option("Back", [] { setPage(Page::Main); });
                MWGui::SortFilterItemModel& model = buy ? *trade->mSortModel : *inv->mSortModel;
                for (size_t i = 0; i < model.getItemCount(); ++i)
                {
                    const MWGui::ItemStack stack = model.getItem(i);
                    const size_t source = model.mapToSource(i);
                    std::function<void()> action;
                    if (buy)
                        action = [trade, source] {
                            trade->mItemToSell = int(source);
                            trade->sellItem(nullptr, 1);
                        };
                    else
                        action = [inv, source] {
                            inv->mSelectedItem = int(source);
                            inv->sellItem(nullptr, 1);
                        };
                    option(itemName(stack.mBase), std::move(action), stackDetail(stack));
                }
                return true;
            }
            option("Buy...", [] { setPage(Page::Buy); }, "the merchant's items");
            option("Sell...", [] { setPage(Page::Sell); }, "your items");
            button(trade->mOfferButton, trade, &MWGui::TradeWindow::onOfferButtonClicked);
            button(trade->mCancelButton, trade, &MWGui::TradeWindow::onCancelButtonClicked);
            return true;
        }

        // ------------------------------------------------------------ services

        template <class W>
        bool buttonList(W* w, const std::string& title, MyGUI::TextBox* goldLabel,
            const std::vector<MyGUI::Button*>& buttons, void (W::*handler)(MyGUI::Widget*), MyGUI::Button* cancel,
            void (W::*cancelHandler)(MyGUI::Widget*))
        {
            if (w == nullptr)
                return false;
            mView.mTitle = title;
            mView.mBody.push_back(caption(goldLabel));
            for (MyGUI::Button* b : buttons)
                button(b, w, handler);
            button(cancel, w, cancelHandler);
            return true;
        }

        template <class Pair>
        static std::vector<MyGUI::Button*> firsts(const std::vector<Pair>& pairs)
        {
            std::vector<MyGUI::Button*> out;
            for (const auto& pair : pairs)
                out.push_back(pair.first);
            return out;
        }

        bool travel()
        {
            auto* w = window<MWGui::TravelWindow>("Travel");
            return w
                && buttonList(w, "TRAVEL", w->mPlayerGold, w->mDestinationButtons,
                    &MWGui::TravelWindow::onTravelButtonClick, w->mCancelButton,
                    &MWGui::TravelWindow::onCancelButtonClicked);
        }

        bool training()
        {
            auto* w = window<MWGui::TrainingWindow>("Training");
            return w
                && buttonList(w, "TRAINING", w->mPlayerGold, w->mTrainingButtons,
                    &MWGui::TrainingWindow::onTrainingSelected, w->mCancelButton,
                    &MWGui::TrainingWindow::onCancelButtonClicked);
        }

        bool spellBuying()
        {
            auto* w = window<MWGui::SpellBuyingWindow>("SpellBuying");
            return w
                && buttonList(w, "BUY SPELLS", w->mPlayerGold, firsts(w->mSpellButtons),
                    &MWGui::SpellBuyingWindow::onSpellButtonClick, w->mCancelButton,
                    &MWGui::SpellBuyingWindow::onCancelButtonClicked);
        }

        bool repair()
        {
            auto* w = window<MWGui::MerchantRepair>("MerchantRepair");
            return w
                && buttonList(w, "REPAIR", w->mGoldLabel, firsts(w->mButtons),
                    &MWGui::MerchantRepair::onRepairButtonClick, w->mOkButton, &MWGui::MerchantRepair::onOkButtonClick);
        }

        // ------------------------------------------------------------ books, rest, level up, journal

        bool book()
        {
            auto* w = window<MWGui::BookWindow>("Book");
            if (w == nullptr || w->mBook.isEmpty())
                return false;
            mView.mTitle = itemName(w->mBook);
            mView.mBody.push_back(bookText(w->mBook.get<ESM::Book>()->mBase->mText.get()));
            if (w->mTakeButtonShow && w->mTakeButtonAllowed)
                option("Take", [w] { w->onTakeButtonClicked(w->mTakeButton); });
            option("Close", [w] { w->onCloseButtonClicked(w->mCloseButton); });
            return true;
        }

        bool scroll()
        {
            auto* w = window<MWGui::ScrollWindow>("Scroll");
            if (w == nullptr || w->mScroll.isEmpty())
                return false;
            mView.mTitle = itemName(w->mScroll);
            mView.mBody.push_back(bookText(w->mScroll.get<ESM::Book>()->mBase->mText.get()));
            if (w->mTakeButtonShow && w->mTakeButtonAllowed)
                option("Take", [w] { w->onTakeButtonClicked(w->mTakeButton); });
            option("Close", [w] { w->onCloseButtonClicked(w->mCloseButton); });
            return true;
        }

        bool rest()
        {
            auto* w = window<MWGui::WaitDialog>("WaitDialog");
            if (w == nullptr)
                return false;
            mView.mTitle = caption(w->mWaitButton) == "Rest" ? "REST" : "WAIT";
            mView.mBody.push_back(caption(w->mDateTimeText));
            mView.mBody.push_back(caption(w->mRestText));
            if (w->mTimeAdvancer.isRunning())
            {
                mView.mBody.push_back(w->mSleeping ? "Resting..." : "Waiting...");
                return true;
            }
            for (int hours : { 1, 2, 4, 8, 12, 24 })
                option(std::to_string(hours) + (hours == 1 ? " hour" : " hours"), [w, hours] {
                    w->mHourSlider->setScrollPosition(size_t(hours - 1));
                    w->onHourSliderChangedPosition(w->mHourSlider, size_t(hours - 1));
                    w->onWaitButtonClicked(w->mWaitButton);
                });
            button(w->mUntilHealedButton, w, &MWGui::WaitDialog::onUntilHealedButtonClicked);
            button(w->mCancelButton, w, &MWGui::WaitDialog::onCancelButtonClicked);
            return true;
        }

        bool levelUp()
        {
            auto* w = window<MWGui::LevelupDialog>("LevelUpDialog");
            if (w == nullptr)
                return false;
            mView.mTitle = "LEVEL UP: " + caption(w->mLevelText);
            mView.mBody.push_back(caption(w->mLevelDescription));
            mView.mBody.push_back("Choose " + std::to_string(w->mCoinCount) + " attributes, then OK.");
            for (const auto& [id, widgets] : w->mAttributeWidgets)
            {
                const bool chosen = std::find(w->mSpentAttributes.begin(), w->mSpentAttributes.end(), id)
                    != w->mSpentAttributes.end();
                std::string detail = caption(widgets.mValue) + "  " + caption(widgets.mMultiplier);
                if (chosen)
                    detail += "  chosen";
                MyGUI::Button* b = widgets.mButton;
                option(caption(b), [w, b] { w->onAttributeClicked(b); }, detail);
            }
            button(w->mOkButton, w, &MWGui::LevelupDialog::onOkButtonClicked);
            return true;
        }

        void journal()
        {
            mView.mTitle = "JOURNAL";
            mView.mBodyFromEnd = true;
            const auto& entries = MWBase::Environment::get().getJournal()->getEntries();
            const size_t first = entries.size() > 40 ? entries.size() - 40 : 0; // the last 40 entries
            for (size_t i = first; i < entries.size(); ++i)
            {
                const MWDialogue::StampedJournalEntry& entry = entries[i];
                if (!mView.mBody.empty())
                    mView.mBody.emplace_back();
                mView.mBody.push_back("Day " + std::to_string(entry.mDay + 1) + ": " + plainText(entry.getText()));
            }
            if (mView.mBody.empty())
                mView.mBody.push_back("(no entries)");
            option("Close", [] { MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode(); });
        }

        // ------------------------------------------------------------ main menu (START), saves

        void mainMenu()
        {
            MWBase::StateManager* state = MWBase::Environment::get().getStateManager();
            if (sState.mPage == Page::Load)
            {
                mView.mTitle = "LOAD GAME";
                option("Back", [] { setPage(Page::Main); });
                for (auto it = state->characterBegin(); it != state->characterEnd(); ++it)
                {
                    const MWState::Character* character = &*it;
                    for (const MWState::Slot& slot : *character)
                        option(
                            slot.mProfile.mDescription,
                            [character, path = slot.mPath] {
                                setPage(Page::Main);
                                MWBase::Environment::get().getStateManager()->requestLoad(character, path);
                            },
                            slot.mProfile.mPlayerName + ", " + slot.mProfile.mPlayerCellName);
                }
                if (mView.mOptions.size() == 1)
                    mView.mBody.push_back("No saved games.");
                return;
            }
            mView.mTitle = "GAME MENU";
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const bool dead = !player.isEmpty() && player.getClass().getCreatureStats(player).isDead();
            const bool running = state->getState() == MWBase::StateManager::State_Running;
            if (running && !dead)
            {
                option("Return", [] { MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_MainMenu); });
                option(
                    "Save game",
                    [player] {
                        const std::string description = player.isInCell()
                            ? std::string(player.getCell()->getCell()->getDescription())
                            : std::string("3DS save");
                        MWBase::Environment::get().getStateManager()->saveGame(description);
                        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_MainMenu);
                    },
                    "a new save");
            }
            option("Load game", [] { setPage(Page::Load); });
            option("Exit game", [] { MWBase::Environment::get().getStateManager()->requestQuit(); });
        }

        // Menus without a bottom-screen view yet: they can still be closed.
        void unsupported(MWGui::GuiMode mode)
        {
            mView.mTitle = std::string("MENU: ") + guiModeName(mode);
            mView.mBody.push_back("This menu has no 3DS view yet. Close it here or with B.");
            option("Close", [] { MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode(); });
        }

        // Cheap: is there anything to show (a menu or a message box with buttons)?
        static bool anyMenu()
        {
            MWGui::WindowManager& wm = windows();
            return wm.isGuiMode() || (wm.mMessageBoxManager && wm.mMessageBoxManager->isInteractiveMessageBox());
        }

        // Empty title: no menu.
        void build()
        {
            // M8: the message boxes are MyGUI's on the bottom screen now (messagebox.cpp, 3DS placement).
            MWGui::WindowManager& wm = windows();
            if (!wm.isGuiMode() || wm.getGuiModeStack().empty())
            {
                sState = State{};
                return;
            }
            const MWGui::GuiMode mode = wm.getMode();
            if (mode != sState.mMode)
                sState = State{ mode, Page::Main, {} };
            switch (mode)
            {
                case MWGui::GM_Loading:
                case MWGui::GM_LoadingWallpaper:
                    return;
                case MWGui::GM_Dialogue: // M8: MyGUI's dialogue window on the two screens (dialogue.cpp)
                    return;
                case MWGui::GM_Inventory:
                    inventory();
                    return;
                case MWGui::GM_Container: // M8: MyGUI's container window on the top screen, the Inventory tab below
                    return;
                case MWGui::GM_Barter: // MyGUI's barter window on the top screen, the Inventory tab below (test #74)
                    return;
                case MWGui::GM_Travel: // M8: MyGUI's travel window on the bottom screen
                    return;
                case MWGui::GM_Training: // M8: MyGUI's training window, centred on the bottom screen
                case MWGui::GM_SpellCreation: // M8: MyGUI's spellmaking window on the top screen
                    return;
                case MWGui::GM_SpellBuying: // the windows not ported yet: on the top screen, with the cursor (19/10)
                case MWGui::GM_MerchantRepair:
                case MWGui::GM_Alchemy:
                case MWGui::GM_Repair:
                case MWGui::GM_Enchanting:
                case MWGui::GM_Recharge:
                case MWGui::GM_QuickKeysMenu:
                case MWGui::GM_Jail:
                case MWGui::GM_Companion: // the companion's window up there, the Inventory tab below (Give)
                    return;
                case MWGui::GM_Book: // M8: MyGUI's book and scroll windows on the top screen (the scaling layer)
                case MWGui::GM_Scroll:
                    return;
                case MWGui::GM_Rest: // on the top screen, with the cursor (19/10)
                case MWGui::GM_Levelup:
                    return;
                case MWGui::GM_Journal: // MyGUI's journal, a book on the top screen (the books' layer), with the cursor
                    return;
                case MWGui::GM_Map3DS: // MyGUI's map window on the bottom screen (the minimap's tap)
                    return;
                case MWGui::GM_MainMenu: // M8: MyGUI's main menu on the two screens (mainmenu.cpp)
                case MWGui::GM_Name: // M8: the system keyboard (textinput.cpp)
                case MWGui::GM_Race: // M8 phase 2: the character creation windows, MyGUI's on the two screens
                case MWGui::GM_Class:
                case MWGui::GM_ClassPick:
                case MWGui::GM_ClassCreate:
                case MWGui::GM_ClassGenerate:
                case MWGui::GM_Birth:
                case MWGui::GM_Review:
                    return;
                default:
                    break;
            }
            unsupported(mode);
        }
    };

    namespace
    {
#ifdef OPENMW3DS_DEBUG
        // Debug builds log what each menu shows, so a run can be followed from the log.
        void logMenu(const p3ds::MenuView& view)
        {
            std::string options;
            for (const p3ds::MenuView::Option& option : view.mOptions)
                options += (options.empty() ? "" : " | ") + option.mLabel;
            std::string last = view.mBody.empty() ? std::string() : view.mBody.back();
            std::replace(last.begin(), last.end(), '\n', '/');
            P3DS_LOGI("menu shown: %s; options: %.300s; last text: %.160s", view.mTitle.c_str(), options.c_str(),
                last.c_str());
        }
#endif
    }

    void pressMenuOption(int index)
    {
        sPressed = index;
    }

    void refreshMenus(p3ds::DebugBottomScreen& screen)
    {
        static p3ds::MenuView shown;
        static bool showing = false;

        int selection = screen.consumeMenuSelection();
        if (selection < 0)
            std::swap(selection, sPressed);
        if (!showing && selection < 0 && !MenuBridge::anyMenu())
            return; // playing: nothing to build

        MenuBridge menu;
        menu.build();
        // The touched option, if the menu still shows it at that index (the game may have changed it since).
        if (selection >= 0 && showing && selection < int(menu.mActions.size()) && menu.mView.mTitle == shown.mTitle
            && selection < int(shown.mOptions.size()) && menu.mView.mOptions[selection] == shown.mOptions[selection])
        {
            P3DS_LOGI("menu: %s -> %s", menu.mView.mTitle.c_str(), menu.mView.mOptions[selection].mLabel.c_str());
            menu.mActions[selection]();
            menu = MenuBridge{};
            menu.build();
        }

        if (menu.mView.mTitle.empty())
        {
            if (showing)
                screen.setMenu(nullptr);
            showing = false;
            return;
        }
        screen.setMenu(&menu.mView);
#ifdef OPENMW3DS_DEBUG
        if (!showing || !(menu.mView == shown))
            logMenu(menu.mView);
#endif
        shown = std::move(menu.mView);
        showing = true;
    }

    const char* guiModeName(MWGui::GuiMode mode)
    {
        switch (mode)
        {
            case MWGui::GM_Inventory:
                return "inventory";
            case MWGui::GM_Container:
                return "container";
            case MWGui::GM_Companion:
                return "companion";
            case MWGui::GM_MainMenu:
                return "main menu";
            case MWGui::GM_Journal:
                return "journal";
            case MWGui::GM_Scroll:
            case MWGui::GM_Book:
                return "book";
            case MWGui::GM_Dialogue:
                return "dialogue";
            case MWGui::GM_Barter:
                return "barter";
            case MWGui::GM_Rest:
                return "rest";
            case MWGui::GM_Levelup:
                return "level up";
            case MWGui::GM_Loading:
            case MWGui::GM_LoadingWallpaper:
                return "loading";
            case MWGui::GM_QuickKeysMenu:
                return "quick keys";
            case MWGui::GM_Travel:
                return "travel";
            case MWGui::GM_Training:
                return "training";
            case MWGui::GM_SpellBuying:
                return "spell buying";
            case MWGui::GM_MerchantRepair:
                return "repair";
            case MWGui::GM_Alchemy:
                return "alchemy";
            case MWGui::GM_Repair:
                return "repair (item)";
            case MWGui::GM_Recharge:
                return "recharge";
            case MWGui::GM_SpellCreation:
                return "spellmaking";
            case MWGui::GM_Enchanting:
                return "enchanting";
            case MWGui::GM_Jail:
                return "jail";
            case MWGui::GM_Map3DS:
                return "map";
            case MWGui::GM_None:
                return "message";
            default:
                return "service";
        }
    }
}
