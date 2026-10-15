// Nintendo 3DS HUD (see hud3ds.hpp).

#include "hud3ds.hpp"

#include <array>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_Gui.h>
#include <MyGUI_RenderManager.h>

#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwgui/hud.hpp"
#include "../mwgui/itemwidget.hpp"
#include "../mwgui/quickkeysmenu.hpp"
#include "../mwgui/windowmanagerimp.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

namespace omw3ds
{
    namespace
    {
        // Bottom screen (canvas): the panel's frame is 4 px; the target panel lies between the boxes (bottom-screen
        // y 92) and the quick keys (y 196), across the whole width.
        constexpr int kBottomX = 40;
        constexpr int kBottomY = 240;
        constexpr int kSlots = 9;
    }

    MyGUI::IntCoord targetPanel3DS()
    {
        return MyGUI::IntCoord(kBottomX + 4, kBottomY + 92, 312, 104);
    }

    namespace
    {
        struct Slot
        {
            MyGUI::Button* mButton = nullptr;
            MWGui::ItemWidget* mIcon = nullptr;
            ESM::QuickKeys::Type mType = ESM::QuickKeys::Type::Unassigned;
            ESM::RefId mId;
            bool mShown = false; // mType/mId are on screen
        };

        std::array<Slot, kSlots> sSlots;
        const MWGui::HUD* sBoundTo = nullptr;
    }

    // Reads OpenMW's quick keys (a friend of WindowManager and QuickKeysMenu: OMW3DS_HUD_FRIEND).
    struct Hud3DS
    {

        static MWGui::WindowManager& windows()
        {
            return static_cast<MWGui::WindowManager&>(*MWBase::Environment::get().getWindowManager());
        }

        static void onSlotClicked(MyGUI::Widget* sender)
        {
            for (int i = 0; i < kSlots; ++i)
                if (sSlots[i].mButton == sender)
                    windows().activateQuickKey(i + 1);
        }

        static bool bind()
        {
            MWGui::HUD* hud = windows().mHud;
            if (hud == nullptr)
                return false;
            if (hud == sBoundTo)
                return true;
            for (int i = 0; i < kSlots; ++i)
            {
                Slot& slot = sSlots[i];
                slot = Slot{};
                const std::string n = std::to_string(i + 1);
                if (hud->getWidget("QuickKey3DS" + n) == nullptr)
                    return false; // OpenMW's own HUD layout (no ui3ds data directory)
                hud->getWidget(slot.mButton, "QuickKey3DS" + n);
                hud->getWidget(slot.mIcon, "QuickKey3DSIcon" + n);
                slot.mButton->eventMouseButtonClick += MyGUI::newDelegate(&Hud3DS::onSlotClicked);
            }
            sBoundTo = hud;
            return true;
        }

        static MWWorld::Ptr findItem(const ESM::RefId& id)
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);
            for (MWWorld::ContainerStoreIterator it = store.begin(); it != store.end(); ++it)
                if (it->getCellRef().getRefId() == id)
                    return *it;
            return MWWorld::Ptr();
        }

        // As QuickKeysMenu shows a spell: the first effect's big icon in the magic frame.
        static void showSpell(MWGui::ItemWidget& icon, const ESM::RefId& spellId)
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::Spell* spell = store.get<ESM::Spell>().search(spellId);
            if (spell == nullptr || spell->mEffects.mList.empty())
                return;
            const ESM::MagicEffect* effect
                = store.get<ESM::MagicEffect>().search(spell->mEffects.mList.front().mData.mEffectID);
            if (effect == nullptr)
                return;
            const VFS::Path::Normalized path = Misc::ResourceHelpers::correctBigIconPath(
                effect->mIcon.getNormalized(), *MWBase::Environment::get().getResourceSystem()->getVFS());
            float scale = 1.f;
            if (MyGUI::ITexture* frame
                = MyGUI::RenderManager::getInstance().getTexture("textures\\menu_icon_select_magic.dds"))
                scale = frame->getHeight() / 64.f;
            const int diameter = static_cast<int>(44 * scale);
            icon.setFrame("textures\\menu_icon_select_magic.dds", MyGUI::IntCoord(0, 0, diameter, diameter));
            icon.setIcon(path);
        }

        static void update()
        {
            if (!bind())
                return;
            const MWGui::QuickKeysMenu* menu = windows().mQuickKeysMenu;
            if (menu == nullptr || menu->mKey.size() < kSlots)
                return;
            for (int i = 0; i < kSlots; ++i)
            {
                Slot& slot = sSlots[i];
                const auto& key = menu->mKey[i];
                if (slot.mShown && slot.mType == key.type && slot.mId == key.id)
                    continue;
                slot.mType = key.type;
                slot.mId = key.id;
                slot.mShown = true;
                slot.mIcon->setItem(MWWorld::Ptr());
                switch (key.type)
                {
                    case ESM::QuickKeys::Type::Item:
                    case ESM::QuickKeys::Type::MagicItem:
                        slot.mIcon->setItem(findItem(key.id));
                        break;
                    case ESM::QuickKeys::Type::Magic:
                        showSpell(*slot.mIcon, key.id);
                        break;
                    case ESM::QuickKeys::Type::HandToHand:
                        slot.mIcon->setIcon(VFS::Path::Normalized("icons/k/stealth_handtohand.dds"));
                        break;
                    default:
                        break;
                }
            }
        }
    };

    void updateHud3DS()
    {
        Hud3DS::update();
    }
}
