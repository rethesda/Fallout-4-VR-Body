#include "Pipboy.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "Config.h"
#include "ConfigurationMode.h"
#include "FRIK.h"
#include "HandPose.h"
#include "utils.h"
#include "common/CommonUtils.h"
#include "common/Logger.h"
#include "common/Quaternion.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/scaleformUtils.h"
#include "f4vr/VRControllersManager.h"

using namespace RE::Scaleform;
using namespace std::chrono;
using namespace common;

namespace
{
    bool isPrimaryTriggerPressed()
    {
        return f4vr::VRControllers.isPressed(vr::k_EButton_SteamVR_Trigger, f4vr::Hand::Primary);
    }

    bool isAButtonPressed()
    {
        return f4vr::VRControllers.isPressed(vr::k_EButton_A, f4vr::Hand::Primary);
    }

    bool isBButtonPressed()
    {
        return f4vr::VRControllers.isPressed(vr::k_EButton_ApplicationMenu, f4vr::Hand::Primary);
    }

    bool isPrimaryGripPressHeldDown()
    {
        return f4vr::VRControllers.isPressHeldDown(vr::k_EButton_Grip, f4vr::Hand::Primary);
    }

    bool isWorldMapVisible(const GFx::Movie* root)
    {
        return f4vr::isElementVisible(root, "root.Menu_mc.CurrentPage.WorldMapHolder_mc");
    }

    std::string getCurrentMapPath(const GFx::Movie* root, const std::string& suffix = "")
    {
        return (isWorldMapVisible(root) ? "root.Menu_mc.CurrentPage.WorldMapHolder_mc" : "root.Menu_mc.CurrentPage.LocalMapHolder_mc") + suffix;
    }

    /**
     * Is context menu message box popup is visible or not. Only one can be visible at a time.
     */
    bool isMessageHolderVisible(const GFx::Movie* root)
    {
        return f4vr::isElementVisible(root, "root.Menu_mc.CurrentPage.MessageHolder_mc")
            || f4vr::isElementVisible(root, "root.Menu_mc.CurrentPage.QuestsTab_mc.MessageHolder_mc");
    }

    bool isQuestTabVisibleOnDataPage(const GFx::Movie* root)
    {
        return f4vr::isElementVisible(root, "root.Menu_mc.CurrentPage.QuestsTab_mc");
    }

    bool isQuestTabObjectiveListEnabledOnDataPage(const GFx::Movie* root)
    {
        GFx::Value var;
        return root->GetVariable(&var, "root.Menu_mc.CurrentPage.QuestsTab_mc.ObjectivesList_mc.selectedIndex") && var.GetType() == GFx::Value::ValueType::kInt && var.GetInt() > -
            1;
    }

    bool isWorkshopsTabVisibleOnDataPage(const GFx::Movie* root)
    {
        return f4vr::isElementVisible(root, "root.Menu_mc.CurrentPage.WorkshopsTab_mc");
    }

    void triggerHeptic()
    {
        f4vr::VRControllers.triggerHaptic(f4vr::Hand::Primary, 0.001f);
    }
}

namespace frik
{
    /**
     * Turn on the Pipboy and set the status flags.
     */
    void Pipboy::turnOn()
    {
        _pipboyStatus = true;
        _isOperatingPipboy = true;
        _stickybpip = false;
        // err... without this line the pipboy screen is not visible...
        f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 1.0;
        turnPipBoyOn();
    }

    void Pipboy::swapPipboy()
    {
        _pipboyStatus = false;
        _pipTimer = 0;
    }

    void Pipboy::onSetNodes()
    {
        if (!_skelly) {
            return;
        }
        if (RE::NiNode* screenNode = f4vr::getPlayerNodes()->ScreenNode) {
            if (const RE::NiAVObject* screen = f4vr::findAVObject(screenNode, std::string("Screen:0"))) {
                _pipboyScreenPrevFrame = screen->world;
            }
        }
    }

    /**
     * Run Pipboy mesh replacement if not already done (or forced) to the configured meshes either holo or screen.
     */
    /// <param name="force">true - run mesh replace, false - only if not previously replaced</param>
    void Pipboy::replaceMeshes(const bool force)
    {
        if (force || !meshesReplaced) {
            if (g_config.isHoloPipboy == 0) {
                replaceMeshes("HoloEmitter", "Screen");
            } else if (g_config.isHoloPipboy == 1) {
                replaceMeshes("Screen", "HoloEmitter");
            }
        }
    }

    /**
     * Executed every frame to update the Pipboy location and handle interaction with pipboy config UX.
     * TODO: refactor into separate functions for each functionality
     */
    void Pipboy::onFrameUpdate()
    {
        replaceMeshes(false);

        operatePipBoy();

        pipboyManagement();

        dampenPipboyScreen();

        if (f4vr::isInPowerArmor()) {
            return;
        }

        //Hide some Pipboy related meshes on exit of Power Armor if they're not hidden
        RE::NiNode* hideNode;
        g_config.isHoloPipboy
            ? hideNode = f4vr::findNode(f4vr::getWorldRootNode(), "Screen")
            : hideNode = f4vr::findNode(f4vr::getWorldRootNode(), "HoloEmitter");
        if (hideNode) {
            if (fNotEqual(hideNode->local.scale, 0)) {
                hideNode->flags.flags |= 0x1;
                hideNode->local.scale = 0;
            }
        }

        // sets 3rd Person Pipboy Scale
        if (const auto pipboy3Rd = f4vr::findNode(f4vr::getWorldRootNode(), "PipboyBone")) {
            pipboy3Rd->local.scale = g_config.pipBoyScale;
        }
    }

    void Pipboy::fixMissingScreen()
    {
        const auto pn = f4vr::getPlayerNodes();
        if (const auto screenNode = pn->ScreenNode) {
            const std::string screenName("Screen:0");
            const RE::NiAVObject* newScreen = f4vr::findAVObject(screenNode, screenName);

            if (!newScreen) {
                pn->ScreenNode->DetachChildAt(0);

                newScreen = f4vr::findAVObject(pn->PipboyRoot_nif_only_node, screenName)->parent;
                f4vr::addNode(reinterpret_cast<uint64_t>(&pn->ScreenNode), newScreen);
            }
        }
    }

    /**
     * Handle replacing of Pipboy meshes on the arm with either screen or holo emitter.
     */
    void Pipboy::replaceMeshes(const std::string& itemHide, const std::string& itemShow)
    {
        const auto pn = f4vr::getPlayerNodes();
        RE::NiNode* pipParent = f4vr::find1StChildNode(pn->SecondaryWandNode, "PipboyParent");
        if (!pipParent) {
            meshesReplaced = false;
            return;
        }

        const auto pipboyRoot = f4vr::find1StChildNode(pipParent, "PipboyRoot_NIF_ONLY");
        const auto pipboyReplacetNode = vrui::loadNifFromFile(g_config.isHoloPipboy ? "Data/Meshes/FRIK/HoloPipboyVR.nif" : "Data/Meshes/FRIK/PipboyVR.nif");
        if (pipboyReplacetNode && pipboyRoot) {
            const auto newScreen = f4vr::findAVObject(pipboyReplacetNode, "Screen:0")->parent;
            if (!newScreen) {
                meshesReplaced = false;
                return;
            }

            pipParent->DetachChild(pipboyRoot);
            pipParent->AttachChild(pipboyReplacetNode, true);

            pn->ScreenNode->DetachChildAt(0);
            // using native function here to attach the new screen as too lazy to fully reverse what it's doing and it works fine.
            f4vr::addNode(reinterpret_cast<uint64_t>(&pn->ScreenNode), newScreen);
            pn->PipboyRoot_nif_only_node = pipboyReplacetNode;
        }

        meshesReplaced = true;

        static std::string wandPipName("PipboyRoot");
        if (const auto pbRoot = f4vr::findAVObject(pn->SecondaryWandNode, wandPipName)) {
            pbRoot->local = g_config.getPipboyOffset();
        }

        pn->PipboyRoot_nif_only_node->local.scale = 0.0; //prevents the VRPipboy screen from being displayed on first load whilst PB is off.
        if (const auto hideNode = f4vr::findNode(f4vr::getWorldRootNode(), itemHide.c_str())) {
            hideNode->flags.flags |= 0x1;
            hideNode->local.scale = 0;
        }
        if (const auto showNode = f4vr::findNode(f4vr::getWorldRootNode(), itemShow.c_str())) {
            showNode->flags.flags &= 0xfffffffffffffffe;
            showNode->local.scale = 1;
        }

        logger::info("Pipboy Meshes replaced! Hide: {}, Show: {}", itemHide.c_str(), itemShow.c_str());
    }

    /**
     * See documentation: https://github.com/rollingrock/Fallout-4-VR-Body/wiki/Development-%E2%80%90-Pipboy-Controls
     */
    void Pipboy::operatePipBoy()
    {
        if (f4vr::getFirstPersonSkeleton() == nullptr) {
            return;
        }

        RE::NiPoint3 finger;
        const RE::NiAVObject* pipboy;
        RE::NiAVObject* pipboyTrans;
        g_config.leftHandedPipBoy
            ? finger = _skelly->getBoneWorldTransform("LArm_Finger23").translate
            : finger = _skelly->getBoneWorldTransform("RArm_Finger23").translate;
        g_config.leftHandedPipBoy
            ? pipboy = f4vr::findNode(_skelly->getRightArm().shoulder, "PipboyRoot")
            : pipboy = f4vr::findNode(_skelly->getLeftArm().shoulder, "PipboyRoot");
        if (pipboy == nullptr) {
            return;
        }

        const auto pipOnButtonPressed = (g_config.pipBoyButtonArm
            ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).ulButtonPressed
            : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).ulButtonPressed) & vr::ButtonMaskFromId(
            static_cast<vr::EVRButtonId>(g_config.pipBoyButtonID));
        const auto pipOffButtonPressed = (g_config.pipBoyButtonOffArm
            ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).ulButtonPressed
            : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).ulButtonPressed) & vr::ButtonMaskFromId(
            static_cast<vr::EVRButtonId>(g_config.pipBoyButtonOffID));

        // check off button
        if (pipOffButtonPressed && !_stickyoffpip) {
            if (_pipboyStatus) {
                _pipboyStatus = false;
                turnPipBoyOff();
                g_frik.closePipboyConfigurationModeActive();
                if (_isWeaponinHand) {
                    _weaponStateDetected = false;
                }
                disablePipboyHandPose();
                f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 0.0;
                logger::info("Disabling Pipboy with button");
                _stickyoffpip = true;
            }
        } else if (!pipOffButtonPressed) {
            // guard so we don't constantly toggle the pip boy off every frame
            _stickyoffpip = false;
        }

        /* Refactored this part of the code so that turning on the wrist based Pipboy works the same way as the 'Projected Pipboy'. It works on button release rather than press,
        this enables us to determine if the button was held for a short or long press by the status of the '_controlSleepStickyT' bool. If it is still set to true on button release
        then we know the button was a short press, if it is set to false we know it was a long press. Long press = torch on / off, Short Press = Pipboy enable.
        */

        if (pipOnButtonPressed && !_stickybpip && !_isOperatingPipboy) {
            _stickybpip = true;
            _controlSleepStickyT = true;
            std::thread t5(&Pipboy::secondaryTriggerSleep, this, 300); // switches a bool to false after 150ms
            t5.detach();
        } else if (!pipOnButtonPressed) {
            if (_controlSleepStickyT && _stickybpip && (!g_config.pipboyOpenWhenLookAt || isLookingAtPipBoy())) {
                // if bool is still set to true on control release we know it was a short press.
                _pipboyStatus = true;
                f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 1.0;
                turnPipBoyOn();
                setPipboyHandPose();
                _isOperatingPipboy = true;
                logger::info("Enabling Pipboy with button");
                _stickybpip = false;
            } else {
                // guard so we don't constantly toggle the pip boy every frame
                _stickybpip = false;
            }
        }

        if (!isLookingAtPipBoy()) {
            _startedLookingAtPip = 0;
            const vr::VRControllerAxis_t movingStick = g_config.pipBoyButtonArm > 0
                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[0]
                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[0];
            const vr::VRControllerAxis_t lookingStick = f4vr::isLeftHandedMode()
                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[0]
                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[0];
            const auto timeElapsed = static_cast<int>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() - _lastLookingAtPip);
            const bool closeLookingWayWithDelay = g_config.pipboyCloseWhenLookAway && _pipboyStatus
                && !g_frik.isPipboyConfigurationModeActive()
                && timeElapsed > g_config.pipBoyOffDelay;
            const bool closeLookingWayWithMovement = g_config.pipboyCloseWhenMovingWhileLookingAway && _pipboyStatus
                && !g_frik.isPipboyConfigurationModeActive()
                && (fNotEqual(movingStick.x, 0, 0.3f) || fNotEqual(movingStick.y, 0, 0.3f)
                    || fNotEqual(lookingStick.x, 0, 0.3f) || fNotEqual(lookingStick.y, 0, 0.3f));

            if (closeLookingWayWithDelay || closeLookingWayWithMovement) {
                _pipboyStatus = false;
                turnPipBoyOff();
                f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 0.0;
                if (_isWeaponinHand) {
                    _weaponStateDetected = false;
                }
                disablePipboyHandPose();
                _isOperatingPipboy = false;
            }
            return;
        }

        if (_pipboyStatus) {
            _lastLookingAtPip = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        } else if (g_config.pipboyOpenWhenLookAt) {
            if (_startedLookingAtPip == 0) {
                _startedLookingAtPip = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            } else {
                const auto timeElapsed = static_cast<int>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() - _startedLookingAtPip);
                if (timeElapsed > g_config.pipBoyOnDelay) {
                    _pipboyStatus = true;
                    f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 1.0;
                    turnPipBoyOn();
                    setPipboyHandPose();
                    _isOperatingPipboy = true;
                    _startedLookingAtPip = 0;
                }
            }
        }

        //Why not enable both? So I commented out....

        //if (g_config.pipBoyButtonMode) // If g_config.pipBoyButtonMode, don't check touch
        //return;

        //Virtual Power Button Code
        static std::string pwrButtonTrans("PowerTranslate");
        g_config.leftHandedPipBoy
            ? pipboy = f4vr::findNode(_skelly->getRightArm().shoulder, "PowerDetect")
            : pipboy = f4vr::findNode(_skelly->getLeftArm().shoulder, "PowerDetect");
        g_config.leftHandedPipBoy
            ? pipboyTrans = f4vr::findAVObject(_skelly->getRightArm().forearm3, pwrButtonTrans)
            : pipboyTrans = f4vr::findAVObject(_skelly->getLeftArm().forearm3, pwrButtonTrans);
        if (!pipboyTrans || !pipboy) {
            return;
        }
        float distance = vec3Len(finger - pipboy->world.translate);
        if (distance > 2.0) {
            _pipTimer = 0;
            _stickypip = false;
            pipboyTrans->local.translate.z = 0.0;
        } else {
            if (_pipTimer < 2) {
                _stickypip = false;
                _pipTimer++;
            } else {
                const float fz = 0 - (2.0f - distance);
                if (fz >= -0.14 && fz <= 0.0) {
                    pipboyTrans->local.translate.z = fz;
                }
                if (pipboyTrans->local.translate.z < -0.10 && !_stickypip) {
                    _stickypip = true;
                    triggerHeptic();
                    if (_pipboyStatus) {
                        _pipboyStatus = false;
                        turnPipBoyOff();
                        g_frik.closePipboyConfigurationModeActive();
                        f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 0.0;
                    } else {
                        _pipboyStatus = true;
                        f4vr::getPlayerNodes()->PipboyRoot_nif_only_node->local.scale = 1.0;
                        turnPipBoyOn();
                    }
                }
            }
        }
        //Virtual Light Button Code
        static std::string lhtButtontrans("LightTranslate");
        g_config.leftHandedPipBoy
            ? pipboy = f4vr::findNode(_skelly->getRightArm().shoulder, "LightDetect")
            : pipboy = f4vr::findNode(_skelly->getLeftArm().shoulder, "LightDetect");
        g_config.leftHandedPipBoy
            ? pipboyTrans = f4vr::findAVObject(_skelly->getRightArm().forearm3, lhtButtontrans)
            : pipboyTrans = f4vr::findAVObject(_skelly->getLeftArm().forearm3, lhtButtontrans);
        if (!pipboyTrans || !pipboy) {
            return;
        }
        distance = vec3Len(finger - pipboy->world.translate);
        if (distance > 2.0) {
            stickyPBlight = false;
            pipboyTrans->local.translate.z = 0.0;
        } else if (distance <= 2.0) {
            const float fz = 0 - (2.0f - distance);
            if (fz >= -0.2 && fz <= 0.0) {
                pipboyTrans->local.translate.z = fz;
            }
            if (pipboyTrans->local.translate.z < -0.14 && !stickyPBlight) {
                stickyPBlight = true;
                triggerHeptic();
                if (!_pipboyStatus) {
                    f4vr::togglePipboyLight(f4vr::getPlayer());
                }
            }
        }
        //Virtual Radio Button Code
        static std::string radioButtontrans("RadioTranslate");
        g_config.leftHandedPipBoy
            ? pipboy = f4vr::findNode(_skelly->getRightArm().shoulder, "RadioDetect")
            : pipboy = f4vr::findNode(_skelly->getLeftArm().shoulder, "RadioDetect");
        g_config.leftHandedPipBoy
            ? pipboyTrans = f4vr::findAVObject(_skelly->getRightArm().forearm3, radioButtontrans)
            : pipboyTrans = f4vr::findAVObject(_skelly->getLeftArm().forearm3, radioButtontrans);
        if (!pipboyTrans || !pipboy) {
            return;
        }
        distance = vec3Len(finger - pipboy->world.translate);
        if (distance > 2.0) {
            stickyPBRadio = false;
            pipboyTrans->local.translate.y = 0.0;
        } else if (distance <= 2.0) {
            const float fz = 0 - (2.0f - distance);
            if (fz >= -0.15 && fz <= 0.0) {
                pipboyTrans->local.translate.y = fz;
            }
            if (pipboyTrans->local.translate.y < -0.12 && !stickyPBRadio) {
                stickyPBRadio = true;
                triggerHeptic();
                if (!_pipboyStatus) {
                    if (f4vr::isPlayerRadioEnabled()) {
                        turnPlayerRadioOn(false);
                    } else {
                        turnPlayerRadioOn(true);
                    }
                }
            }
        }
    }

    /**
     * Execute operation on the currently active Pipboy UI.
     * First thing is to detect is a message box is visible (context menu options) as message box and main lists are active at the
     * same time. So, if the message box is visible we will only operate on the message box list and not the main list.
     * @param root - Pipboy Scaleform UI root
     * @param triggerPressed - true if operate as trigger is pressed regardless of controller input (used for "skeleton control")
     */
    void Pipboy::handlePrimaryControllerOperation(GFx::Movie* root, bool triggerPressed)
    {
        // page level navigation
        const bool gripPressHeldDown = isPrimaryGripPressHeldDown();
        if (!gripPressHeldDown && isAButtonPressed()) {
            gotoPrevPage(root);
            return;
        }
        if (!gripPressHeldDown && isBButtonPressed()) {
            gotoNextPage(root);
            return;
        }

        // include actual controller check
        triggerPressed = triggerPressed || isPrimaryTriggerPressed();

        // Context menu message box handling
        if (triggerPressed && isMessageHolderVisible(root)) {
            triggerHeptic();
            f4vr::doOperationOnScaleformMessageHolderList(root, "root.Menu_mc.CurrentPage.MessageHolder_mc", f4vr::ScaleformListOp::Select);
            f4vr::doOperationOnScaleformMessageHolderList(root, "root.Menu_mc.CurrentPage.QuestsTab_mc.MessageHolder_mc", f4vr::ScaleformListOp::Select);
            // prevent affecting the main list if message box is visible
            return;
        }

        // Specific handling by current page
        switch (f4vr::getScaleformInt(root, "root.Menu_mc.DataObj._CurrentPage").value_or(-1)) {
        case 0:
            handlePrimaryControllerOperationOnStatusPage(root, triggerPressed);
            break;
        case 1:
            handlePrimaryControllerOperationOnInventoryPage(root, triggerPressed);
            break;
        case 2:
            handlePrimaryControllerOperationOnDataPage(root, triggerPressed);
            break;
        case 3:
            handlePrimaryControllerOperationOnMapPage(root, triggerPressed);
            break;
        case 4:
            handlePrimaryControllerOperationOnRadioPage(root, triggerPressed);
            break;
        default: ;
        }
    }

    void Pipboy::gotoPrevPage(GFx::Movie* root)
    {
        root->Invoke("root.Menu_mc.gotoPrevPage", nullptr, nullptr, 0);
    }

    void Pipboy::gotoNextPage(GFx::Movie* root)
    {
        root->Invoke("root.Menu_mc.gotoNextPage", nullptr, nullptr, 0);
    }

    void Pipboy::gotoPrevTab(GFx::Movie* root)
    {
        triggerHeptic();
        root->Invoke("root.Menu_mc.gotoPrevTab", nullptr, nullptr, 0);
    }

    void Pipboy::gotoNextTab(GFx::Movie* root)
    {
        triggerHeptic();
        root->Invoke("root.Menu_mc.gotoNextTab", nullptr, nullptr, 0);
    }

    /**
     * Execute moving selection on the currently active list up or down.
     * Whatever the list found to exist it will be moved, mostly we can "try" to move any of the lists and only the one that exists will be moved.
     * On DATA page all 3 tabs can exist at the same time, so we need to check which one is visible to prevent working on a hidden one.
     * First thing is to detect is a message box is visible (context menu options) as message box and main lists are active at the
     * same time. So, if the message box is visible we will only operate on the message box list and not the main list.
     */
    void Pipboy::moveListSelectionUpDown(GFx::Movie* root, const bool moveUp)
    {
        triggerHeptic();

        const auto listOp = moveUp ? f4vr::ScaleformListOp::MoveUp : f4vr::ScaleformListOp::MoveDown;

        if (isMessageHolderVisible(root)) {
            f4vr::doOperationOnScaleformMessageHolderList(root, "root.Menu_mc.CurrentPage.MessageHolder_mc", listOp);
            f4vr::doOperationOnScaleformMessageHolderList(root, "root.Menu_mc.CurrentPage.QuestsTab_mc.MessageHolder_mc", listOp);
            // prevent affecting the main list if message box is visible
            return;
        }

        // Inventory, Radio, Special, and Perks tabs
        f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.List_mc", listOp);
        f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.SPECIALTab_mc.List_mc", listOp);
        f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.PerksTab_mc.List_mc", listOp);

        // Quest, Workshop, and Stats tabs exist at the same time, need to check which one is visible
        if (isQuestTabVisibleOnDataPage(root)) {
            // Quests tab has 2 lists for the main quests and quest objectives
            const char* listPath = isQuestTabObjectiveListEnabledOnDataPage(root)
                ? "root.Menu_mc.CurrentPage.QuestsTab_mc.ObjectivesList_mc"
                : "root.Menu_mc.CurrentPage.QuestsTab_mc.QuestsList_mc";
            f4vr::doOperationOnScaleformList(root, listPath, listOp);
        } else if (isWorkshopsTabVisibleOnDataPage(root)) {
            f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.WorkshopsTab_mc.List_mc", listOp);
        } else {
            f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.StatsTab_mc.CategoryList_mc", listOp);
        }
    }

    void Pipboy::handlePrimaryControllerOperationOnStatusPage(GFx::Movie* root, const bool triggerPressed)
    {
        if (triggerPressed) {
            triggerHeptic();
            f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.SPECIALTab_mc.List_mc", f4vr::ScaleformListOp::Select);
            f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.PerksTab_mc.List_mc", f4vr::ScaleformListOp::Select);
        }
    }

    void Pipboy::handlePrimaryControllerOperationOnInventoryPage(GFx::Movie* root, const bool triggerPressed)
    {
        if (triggerPressed) {
            triggerHeptic();
            f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.List_mc", f4vr::ScaleformListOp::Select);
        }
    }

    /**
     * On DATA page all 3 tabs can exist at the same time, so we need to check which one is visible to prevent working on a hidden one.
     */
    void Pipboy::handlePrimaryControllerOperationOnDataPage(GFx::Movie* root, const bool triggerPressed)
    {
        if (triggerPressed) {
            triggerHeptic();
            if (isQuestTabVisibleOnDataPage(root)) {
                f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.QuestsTab_mc.QuestsList_mc", f4vr::ScaleformListOp::Select);
            } else {
                // open quest in map
                f4vr::invokeScaleformProcessUserEvent(root, "root.Menu_mc.CurrentPage.WorkshopsTab_mc", "XButton");
            }
        }
    }

    /**
     * For handling map faster travel or marker setting we need to know if fast travel can be done using "bCanFastTravel"
     * and then let the Pipboy code handle the rest by sending the right event to the currently visible map (world/local).
     */
    void Pipboy::handlePrimaryControllerOperationOnMapPage(GFx::Movie* root, const bool triggerPressed)
    {
        if (isPrimaryGripPressHeldDown()) {
            if (triggerPressed) {
                // switch world/local maps
                triggerHeptic();
                f4vr::invokeScaleformProcessUserEvent(root, "root.Menu_mc.CurrentPage", "XButton");
            } else {
                // zoom map
                const auto [_, primAxisY] = f4vr::VRControllers.getAxisValue(f4vr::Hand::Primary);
                if (fNotEqual(primAxisY, 0, 0.5f)) {
                    GFx::Value args[1];
                    args[0] = primAxisY / 100.f;
                    root->Invoke(getCurrentMapPath(root, ".ZoomMap").c_str(), nullptr, args, 1);
                }
            }
        } else if (triggerPressed) {
            triggerHeptic();

            // handle fast travel, custom marker
            const char* eventName = f4vr::getScaleformBool(root, getCurrentMapPath(root, ".bCanFastTravel").c_str()) ? "MapHolder:activate_marker" : "MapHolder:set_custom_marker";
            f4vr::invokeScaleformDispatchEvent(root, getCurrentMapPath(root), eventName);
        }
    }

    void Pipboy::handlePrimaryControllerOperationOnRadioPage(GFx::Movie* root, const bool triggerPressed)
    {
        if (triggerPressed) {
            triggerHeptic();
            f4vr::doOperationOnScaleformList(root, "root.Menu_mc.CurrentPage.List_mc", f4vr::ScaleformListOp::Select);
        }
    }

    /**
     * Get Current Pipboy Tab and store it.
     */
    void Pipboy::storeLastPipboyPage(const GFx::Movie* root)
    {
        GFx::Value PBCurrentPage;
        if (root && root->GetVariable(&PBCurrentPage, "root.Menu_mc.DataObj._CurrentPage") && PBCurrentPage.GetType() != GFx::Value::ValueType::kUndefined) {
            _lastPipboyPage = PBCurrentPage.GetUInt();
        }
    }

    void Pipboy::pipboyManagement()
    {
        //Manages all aspects of Virtual Pipboy usage outside of turning the device / radio / torch on or off. Additionally, swaps left hand controls to the right hand.
        bool isInPA = f4vr::isInPowerArmor();
        if (!isInPA) {
            static std::string orbNames[7] = {
                "TabChangeUpOrb", "TabChangeDownOrb", "PageChangeUpOrb", "PageChangeDownOrb", "ScrollItemsUpOrb", "ScrollItemsDownOrb", "SelectItemsOrb"
            };
            bool helmetHeadLamp = isArmorHasHeadLamp();
            bool lightOn = f4vr::isPipboyLightOn(f4vr::getPlayer());
            bool radioOn = f4vr::isPlayerRadioEnabled();
            float radFreq = f4vr::getPlayerRadioFreq() - 23;
            static std::string pwrButtonOn("PowerButton_mesh:2");
            static std::string pwrButtonOff("PowerButton_mesh:off");
            static std::string lhtButtonOn("LightButton_mesh:2");
            static std::string lhtButtonOff("LightButton_mesh:off");
            static std::string radButtonOn("RadioOn");
            static std::string radButtonOff("RadioOff");
            static std::string radioNeedle("RadioNeedle_mesh");
            static std::string newModeKnob("ModeKnobDuplicate");
            static std::string originalModeKnob("ModeKnob02");
            RE::NiAVObject* pipbone = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, pwrButtonOn)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, pwrButtonOn);
            RE::NiAVObject* pipbone2 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, pwrButtonOff)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, pwrButtonOff);
            RE::NiAVObject* pipbone3 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, lhtButtonOn)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, lhtButtonOn);
            RE::NiAVObject* pipbone4 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, lhtButtonOff)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, lhtButtonOff);
            RE::NiAVObject* pipbone5 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, radButtonOn)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, radButtonOn);
            RE::NiAVObject* pipbone6 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, radButtonOff)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, radButtonOff);
            RE::NiAVObject* pipbone7 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, radioNeedle)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, radioNeedle);
            RE::NiAVObject* pipbone8 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, newModeKnob)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, newModeKnob);
            RE::NiAVObject* pipbone9 = g_config.leftHandedPipBoy
                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, originalModeKnob)
                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, originalModeKnob);
            if (!pipbone || !pipbone2 || !pipbone3 || !pipbone4 || !pipbone5 || !pipbone6 || !pipbone7 || !pipbone8) {
                return;
            }
            if (isLookingAtPipBoy()) {
                RE::NiPoint3 finger;
                RE::NiAVObject* pipboy = nullptr;
                g_config.leftHandedPipBoy
                    ? finger = _skelly->getBoneWorldTransform("LArm_Finger23").translate
                    : finger = _skelly->getBoneWorldTransform("RArm_Finger23").translate;
                g_config.leftHandedPipBoy
                    ? pipboy = f4vr::findNode(_skelly->getRightArm().shoulder, "PipboyRoot")
                    : pipboy = f4vr::findNode(_skelly->getLeftArm().shoulder, "PipboyRoot");
                float distance;
                distance = vec3Len(finger - pipboy->world.translate);
                if (distance < g_config.pipboyDetectionRange && !_isOperatingPipboy && !_pipboyStatus) {
                    // Hides Weapon and poses hand for pointing
                    _isOperatingPipboy = true;
                    _isWeaponinHand = f4vr::getPlayer()->actorState.IsWeaponDrawn();
                    setPipboyHandPose();
                }
                if (distance > g_config.pipboyDetectionRange && _isOperatingPipboy && !_pipboyStatus) {
                    // Restores Weapon and releases hand pose
                    _isOperatingPipboy = false;
                    disablePipboyHandPose();
                    if (_isWeaponinHand) {
                        _weaponStateDetected = false;
                    }
                }
            } else if (!isLookingAtPipBoy() && _isOperatingPipboy && !_pipboyStatus) {
                // Catches if you're not looking at the pipboy when your hand moves outside the control area and restores weapon / releases hand pose
                disablePipboyHandPose();
                for (int i = 0; i < 7; i++) {
                    // Remove any stuck helper orbs if Pipboy times out for any reason.
                    RE::NiAVObject* orb = g_config.leftHandedPipBoy
                        ? f4vr::findAVObject(_skelly->getRightArm().forearm3, orbNames[i])
                        : f4vr::findAVObject(_skelly->getLeftArm().forearm3, orbNames[i]);
                    if (orb != nullptr) {
                        orb->local.scale = std::min<float>(orb->local.scale, 0);
                    }
                }
                if (_isWeaponinHand) {
                    _weaponStateDetected = false;
                }
                _isOperatingPipboy = false;
            }
            if (_lastPipboyPage == 4) {
                // fixes broken 'Mode Knob' position when radio tab is selected
                float rotx;
                float roty;
                float rotz;
                getEulerAnglesFromMatrix(pipbone8->local.rotate, &rotx, &roty, &rotz);
                if (rotx < 0.57) {
                    pipbone8->local.rotate = pipbone8->local.rotate * getMatrixFromEulerAngles(-0.05f, 0, 0);
                }
            } else {
                // restores control of the 'Mode Knob' to the Pipboy behaviour file
                pipbone8->local.rotate = pipbone9->local.rotate;
            }
            // Controls Pipboy power light glow (on or off depending on Pipboy state)
            _pipboyStatus ? pipbone->flags.flags &= 0xfffffffffffffffe : pipbone2->flags.flags &= 0xfffffffffffffffe;
            _pipboyStatus ? pipbone->local.scale = 1 : pipbone2->local.scale = 1;
            _pipboyStatus ? pipbone2->flags.flags |= 0x1 : pipbone->flags.flags |= 0x1;
            _pipboyStatus ? pipbone2->local.scale = 0 : pipbone->local.scale = 0;
            // Control switching between hand and head based Pipboy light
            if (lightOn && !helmetHeadLamp) {
                RE::NiAVObject* head = f4vr::findNode(f4vr::getPlayer()->GetActorRootNode(false), "Head");
                if (!head) {
                    return;
                }
                const bool useRightHand = g_config.leftHandedPipBoy || g_config.isPipBoyTorchRightArmMode;
                const auto hand = _skelly->getBoneWorldTransform(useRightHand ? "RArm_Hand" : "LArm_Hand").translate;
                float distance = vec3Len(hand - head->world.translate);
                if (distance < 15.0) {
                    uint64_t pipboyHand = f4vr::VRControllers.getControllerState_DEPRECATED(useRightHand ? f4vr::TrackerType::Right : f4vr::TrackerType::Left).ulButtonPressed;
                    const auto SwitchLightButton = pipboyHand & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_config.switchTorchButton));
                    f4vr::VRControllers.triggerHaptic(useRightHand ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand, 0.1f, 0.1f);
                    // Control switching between hand and head based Pipboy light
                    if (SwitchLightButton && !_SwithLightButtonSticky) {
                        _SwithLightButtonSticky = true;
                        _SwitchLightHaptics = false;
                        f4vr::VRControllers.triggerHaptic(useRightHand ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand, 0.1f);
                        auto LGHT_ATTACH = useRightHand
                            ? f4vr::findNode(_skelly->getRightArm().shoulder, "RArm_Hand")
                            : f4vr::findNode(_skelly->getLeftArm().shoulder, "LArm_Hand");
                        RE::NiNode* lght = g_config.isPipBoyTorchOnArm
                            ? f4vr::find1StChildNode(LGHT_ATTACH, "HeadLightParent")
                            : f4vr::getPlayerNodes()->HeadLightParentNode;
                        if (lght) {
                            auto parentnode = g_config.isPipBoyTorchOnArm ? lght->parent->name : f4vr::getPlayerNodes()->HeadLightParentNode->parent->name;
                            float rotz = g_config.isPipBoyTorchOnArm ? -90 : 90;
                            lght->local.rotate = lght->local.rotate * getMatrixFromEulerAngles(0, 0, degreesToRads(rotz));
                            lght->local.translate.y = g_config.isPipBoyTorchOnArm ? 0 : 4;
                            g_config.isPipBoyTorchOnArm
                                ? lght->parent->DetachChild(lght)
                                : f4vr::getPlayerNodes()->HeadLightParentNode->parent->DetachChild(lght);
                            g_config.isPipBoyTorchOnArm
                                ? f4vr::getPlayerNodes()->HmdNode->AttachChild(lght, true)
                                : LGHT_ATTACH->AttachChild(lght, true);
                            g_config.togglePipBoyTorchOnArm();
                        }
                    }
                    if (!SwitchLightButton) {
                        _SwithLightButtonSticky = false;
                    }
                } else if (distance > 10) {
                    _SwitchLightHaptics = true;
                    _SwithLightButtonSticky = false;
                }
            }
            //Attach light to hand
            if (g_config.isPipBoyTorchOnArm) {
                auto LGHT_ATTACH = g_config.leftHandedPipBoy || g_config.isPipBoyTorchRightArmMode
                    ? f4vr::findNode(_skelly->getRightArm().shoulder, "RArm_Hand")
                    : f4vr::findNode(_skelly->getLeftArm().shoulder, "LArm_Hand");
                if (LGHT_ATTACH) {
                    if (lightOn && !helmetHeadLamp) {
                        RE::NiNode* lght = f4vr::getPlayerNodes()->HeadLightParentNode;
                        auto parentnode = f4vr::getPlayerNodes()->HeadLightParentNode->parent->name;
                        if (parentnode == "HMDNode") {
                            f4vr::getPlayerNodes()->HeadLightParentNode->parent->DetachChild(lght);
                            lght->local.rotate = lght->local.rotate * getMatrixFromEulerAngles(0, 0, degreesToRads(90));
                            lght->local.translate.y = 4;
                            LGHT_ATTACH->AttachChild(lght, true);
                        }
                    }
                    //Restore HeadLight to correct node when light is powered off (to avoid any crashes)
                    else if (!lightOn || helmetHeadLamp) {
                        if (auto lght = f4vr::find1StChildNode(LGHT_ATTACH, "HeadLightParent")) {
                            auto parentnode = lght->parent->name;
                            if (parentnode != "HMDNode") {
                                lght->local.rotate = lght->local.rotate * getMatrixFromEulerAngles(0, 0, degreesToRads(-90));
                                lght->local.translate.y = 0;
                                lght->parent->DetachChild(lght);
                                f4vr::getPlayerNodes()->HmdNode->AttachChild(lght, true);
                            }
                        }
                    }
                }
            }
            // Controls Radio / Light on & off indicators
            lightOn ? pipbone3->flags.flags &= 0xfffffffffffffffe : pipbone4->flags.flags &= 0xfffffffffffffffe;
            lightOn ? pipbone3->local.scale = 1 : pipbone4->local.scale = 1;
            lightOn ? pipbone4->flags.flags |= 0x1 : pipbone3->flags.flags |= 0x1;
            lightOn ? pipbone4->local.scale = 0 : pipbone3->local.scale = 0;
            radioOn ? pipbone5->flags.flags &= 0xfffffffffffffffe : pipbone6->flags.flags &= 0xfffffffffffffffe;
            radioOn ? pipbone5->local.scale = 1 : pipbone6->local.scale = 1;
            radioOn ? pipbone6->flags.flags |= 0x1 : pipbone5->flags.flags |= 0x1;
            radioOn ? pipbone6->local.scale = 0 : pipbone5->local.scale = 0;
            // Controls Radio Needle Position.
            if (radioOn && radFreq != lastRadioFreq) {
                float x = -1 * (radFreq - lastRadioFreq);
                pipbone7->local.rotate = pipbone7->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(x), 0);
                lastRadioFreq = radFreq;
            } else if (!radioOn && lastRadioFreq > 0) {
                float x = lastRadioFreq;
                pipbone7->local.rotate = pipbone7->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(x), 0);
                lastRadioFreq = 0.0;
            }
            // Scale-form code for managing Pipboy menu controls (Virtual and Physical)
            if (_pipboyStatus) {
                std::string pipboyMenu("PipboyMenu");
                auto menu = RE::UI::GetSingleton()->GetMenu(pipboyMenu);
                if (menu != nullptr) {
                    auto root = menu->uiMovie.get();
                    if (root != nullptr) {
                        storeLastPipboyPage(root);
                        static std::string boneNames[7] = {
                            "TabChangeUp", "TabChangeDown", "PageChangeUp", "PageChangeDown", "ScrollItemsUp", "ScrollItemsDown", "SelectButton02"
                        };
                        static std::string transNames[7] = {
                            "TabChangeUpTrans", "TabChangeDownTrans", "PageChangeUpTrans", "PageChangeDownTrans", "ScrollItemsUpTrans", "ScrollItemsDownTrans", "SelectButtonTrans"
                        };
                        float boneDistance[7] = { 2.0f, 2.0f, 2.0f, 2.0f, 1.5f, 1.5f, 2.0f };
                        float transDistance[7] = { 0.6f, 0.6f, 0.6f, 0.6f, 0.1f, 0.1f, 0.4f };
                        float maxDistance[7] = { 1.2f, 1.2f, 1.2f, 1.2f, 1.2f, 1.2f, 0.6f };
                        RE::NiPoint3 finger;
                        // Virtual Controls Code Starts here:
                        g_config.leftHandedPipBoy
                            ? finger = _skelly->getBoneWorldTransform("LArm_Finger23").translate
                            : finger = _skelly->getBoneWorldTransform("RArm_Finger23").translate;
                        for (int i = 0; i < 7; i++) {
                            RE::NiAVObject* bone = g_config.leftHandedPipBoy
                                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, boneNames[i])
                                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, boneNames[i]);
                            auto trans = g_config.leftHandedPipBoy
                                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, transNames[i])
                                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, transNames[i]);
                            if (bone && trans) {
                                float distance = vec3Len(finger - bone->world.translate);
                                if (distance > boneDistance[i]) {
                                    trans->local.translate.z = 0.0;
                                    _PBControlsSticky[i] = false;
                                    RE::NiAVObject* orb = g_config.leftHandedPipBoy
                                        ? f4vr::findAVObject(_skelly->getRightArm().forearm3, orbNames[i])
                                        : f4vr::findAVObject(_skelly->getLeftArm().forearm3, orbNames[i]); //Hide helper Orbs when not near a control surface
                                    if (orb != nullptr) {
                                        orb->local.scale = std::min<float>(orb->local.scale, 0);
                                    }
                                } else if (distance <= boneDistance[i]) {
                                    float fz = boneDistance[i] - distance;
                                    RE::NiAVObject* orb = g_config.leftHandedPipBoy
                                        ? f4vr::findAVObject(_skelly->getRightArm().forearm3, orbNames[i])
                                        : f4vr::findAVObject(_skelly->getLeftArm().forearm3, orbNames[i]); //Show helper Orbs when not near a control surface
                                    if (orb != nullptr) {
                                        orb->local.scale = std::max<float>(orb->local.scale, 1);
                                    }
                                    if (fz > 0.0 && fz < maxDistance[i]) {
                                        trans->local.translate.z = fz;
                                        if (i == 4) {
                                            // Move Scroll Knob Anti-Clockwise when near control surface
                                            static std::string KnobNode = "ScrollItemsKnobRot";
                                            RE::NiAVObject* ScrollKnob = g_config.leftHandedPipBoy
                                                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, KnobNode)
                                                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, KnobNode);
                                            ScrollKnob->local.rotate = ScrollKnob->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(fz), 0);
                                        }
                                        if (i == 5) {
                                            // Move Scroll Knob Clockwise when near control surface
                                            float roty = fz * -1;
                                            static std::string KnobNode = "ScrollItemsKnobRot";
                                            RE::NiAVObject* ScrollKnob = g_config.leftHandedPipBoy
                                                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, KnobNode)
                                                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, KnobNode);
                                            ScrollKnob->local.rotate = ScrollKnob->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(roty), 0);
                                        }
                                    }
                                    if (trans->local.translate.z > transDistance[i] && !_PBControlsSticky[i]) {
                                        _PBControlsSticky[i] = true;
                                        triggerHeptic();
                                        if (i == 0) {
                                            gotoPrevPage(root);
                                        }
                                        if (i == 1) {
                                            gotoNextPage(root);
                                        }
                                        if (i == 2) {
                                            gotoPrevTab(root);
                                        }
                                        if (i == 3) {
                                            gotoNextTab(root);
                                        }
                                        if (i == 4) {
                                            moveListSelectionUpDown(root, true);
                                        }
                                        if (i == 5) {
                                            moveListSelectionUpDown(root, false);
                                        }
                                        if (i == 6) {
                                            handlePrimaryControllerOperation(root, true);
                                        }
                                    }
                                }
                            }
                        }
                        // Mirror Left Stick Controls on Right Stick.
                        if (!g_frik.isPipboyConfigurationModeActive() && g_config.enablePrimaryControllerPipboyUse) {
                            std::string selectnodename = "SelectRotate";
                            auto trans = g_config.leftHandedPipBoy
                                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, selectnodename)
                                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, selectnodename);
                            vr::VRControllerAxis_t doinantHandStick = f4vr::isLeftHandedMode()
                                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[0]
                                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[0];
                            vr::VRControllerAxis_t doinantTrigger = f4vr::isLeftHandedMode()
                                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[1]
                                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[1];
                            vr::VRControllerAxis_t secondaryTrigger = f4vr::isLeftHandedMode()
                                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[1]
                                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[1];
                            uint64_t dominantHand = f4vr::isLeftHandedMode()
                                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).ulButtonPressed
                                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).ulButtonPressed;
                            const auto UIAltSelectButton = dominantHand & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(32)); // Right Touchpad
                            bool isPBMessageBoxVisible = false;
                            // Move Pipboy trigger mesh with controller trigger position.
                            if (trans != nullptr) {
                                if (doinantTrigger.x > 0.00 && secondaryTrigger.x == 0.0) {
                                    trans->local.translate.z = doinantTrigger.x / 3 * -1;
                                } else if (secondaryTrigger.x > 0.00 && doinantTrigger.x == 0.0) {
                                    trans->local.translate.z = secondaryTrigger.x / 3 * -1;
                                } else {
                                    trans->local.translate.z = 0.00;
                                }
                            }
                            isPBMessageBoxVisible = isMessageHolderVisible(root);
                            if (_lastPipboyPage != 3 || isPBMessageBoxVisible) {
                                static std::string KnobNode = "ScrollItemsKnobRot";
                                RE::NiAVObject* ScrollKnob = g_config.leftHandedPipBoy
                                    ? f4vr::findAVObject(_skelly->getRightArm().forearm3, KnobNode)
                                    : f4vr::findAVObject(_skelly->getLeftArm().forearm3, KnobNode);
                                if (doinantHandStick.y > 0.85) {
                                    ScrollKnob->local.rotate = ScrollKnob->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(0.4f), 0);
                                }
                                if (doinantHandStick.y < -0.85) {
                                    ScrollKnob->local.rotate = ScrollKnob->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(-0.4f), 0);
                                }
                            }
                            if (!isPrimaryGripPressHeldDown()) {
                                if (_lastPipboyPage == 3 && !isPBMessageBoxVisible) {
                                    // Map Tab
                                    GFx::Value akArgs[2];
                                    akArgs[0] = doinantHandStick.x * -1;
                                    akArgs[1] = doinantHandStick.y;
                                    if (root->Invoke("root.Menu_mc.CurrentPage.WorldMapHolder_mc.PanMap", nullptr, akArgs, 2)) {} // Move Map
                                    if (root->Invoke("root.Menu_mc.CurrentPage.LocalMapHolder_mc.PanMap", nullptr, akArgs, 2)) {}
                                } else {
                                    if (doinantHandStick.y > 0.85) {
                                        if (!_controlSleepStickyY) {
                                            _controlSleepStickyY = true;
                                            moveListSelectionUpDown(root, true);
                                            std::thread t2(&Pipboy::rightStickYSleep, this, 155);
                                            t2.detach();
                                        }
                                    }
                                    if (doinantHandStick.y < -0.85) {
                                        if (!_controlSleepStickyY) {
                                            _controlSleepStickyY = true;
                                            moveListSelectionUpDown(root, false);
                                            std::thread t2(&Pipboy::rightStickYSleep, this, 155);
                                            t2.detach();
                                        }
                                    }
                                    if (doinantHandStick.x < -0.85) {
                                        if (!_controlSleepStickyX) {
                                            _controlSleepStickyX = true;
                                            gotoPrevTab(root);
                                            std::thread t3(&Pipboy::rightStickXSleep, this, 170);
                                            t3.detach();
                                        }
                                    }
                                    if (doinantHandStick.x > 0.85) {
                                        if (!_controlSleepStickyX) {
                                            _controlSleepStickyX = true;
                                            gotoNextTab(root);
                                            std::thread t3(&Pipboy::rightStickXSleep, this, 170);
                                            t3.detach();
                                        }
                                    }
                                }
                            }

                            handlePrimaryControllerOperation(root, false);

                            if (UIAltSelectButton && !_UIAltSelectSticky) {
                                _UIAltSelectSticky = true;
                                if (root->Invoke("root.Menu_mc.CurrentPage.onMessageButtonPress()", nullptr, nullptr, 0)) {}
                            } else if (!UIAltSelectButton) {
                                _UIAltSelectSticky = false;
                            }
                        } else if (!g_frik.isPipboyConfigurationModeActive() && !g_config.enablePrimaryControllerPipboyUse) {
                            //still move Pipboy trigger mesh even if controls havent been swapped.
                            vr::VRControllerAxis_t secondaryTrigger = f4vr::isLeftHandedMode()
                                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[1]
                                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[1];
                            vr::VRControllerAxis_t offHandStick = f4vr::isLeftHandedMode()
                                ? f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Right).rAxis[0]
                                : f4vr::VRControllers.getControllerState_DEPRECATED(f4vr::TrackerType::Left).rAxis[0];
                            std::string selectnodename = "SelectRotate";
                            auto trans = g_config.leftHandedPipBoy
                                ? f4vr::findAVObject(_skelly->getRightArm().forearm3, selectnodename)
                                : f4vr::findAVObject(_skelly->getLeftArm().forearm3, selectnodename);
                            if (trans != nullptr) {
                                if (secondaryTrigger.x > 0.00) {
                                    trans->local.translate.z = secondaryTrigger.x / 3 * -1;
                                } else {
                                    trans->local.translate.z = 0.00;
                                }
                            }
                            //still move Pipboy scroll knob even if controls haven't been swapped.
                            const bool isPBMessageBoxVisible = isMessageHolderVisible(root);
                            if (_lastPipboyPage != 3 || isPBMessageBoxVisible) {
                                static std::string KnobNode = "ScrollItemsKnobRot";
                                RE::NiAVObject* ScrollKnob = g_config.leftHandedPipBoy
                                    ? f4vr::findAVObject(_skelly->getRightArm().forearm3, KnobNode)
                                    : f4vr::findAVObject(_skelly->getLeftArm().forearm3, KnobNode);
                                if (offHandStick.x > 0.85) {
                                    ScrollKnob->local.rotate = ScrollKnob->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(0.4), 0);
                                }
                                if (offHandStick.x < -0.85) {
                                    ScrollKnob->local.rotate = ScrollKnob->local.rotate * getMatrixFromEulerAngles(0, degreesToRads(-0.4), 0);
                                }
                            }
                        }
                    }
                }
            }
        } else if (isInPA) {
            lastRadioFreq = 0.0; // Ensures Radio needle doesn't get messed up when entering and then exiting Power Armor.
            // Continue to update Pipboy page info when in Power Armor.
            std::string pipboyMenu("PipboyMenu");
            auto menu = RE::UI::GetSingleton()->GetMenu(pipboyMenu);
            if (menu != nullptr) {
                auto root = menu->uiMovie.get();
                storeLastPipboyPage(root);
            }
        }
    }

    void Pipboy::dampenPipboyScreen()
    {
        if (!g_config.dampenPipboyScreen) {
            return;
        }
        if (!_pipboyStatus) {
            _pipboyScreenPrevFrame = f4vr::getPlayerNodes()->ScreenNode->world;
            return;
        }
        RE::NiNode* pipboyScreen = f4vr::getPlayerNodes()->ScreenNode;

        if (pipboyScreen && _pipboyStatus) {
            Quaternion rq, rt;
            // do a spherical interpolation between previous frame and current frame for the world rotation matrix
            const RE::NiTransform prevFrame = _pipboyScreenPrevFrame;
            rq.fromMatrix(prevFrame.rotate);
            rt.fromMatrix(pipboyScreen->world.rotate);
            rq.slerp(1 - g_config.dampenPipboyRotation, rt);
            pipboyScreen->world.rotate = rq.getMatrix();
            // do a linear interpolation between the position from the previous frame to current frame
            RE::NiPoint3 deltaPos = pipboyScreen->world.translate - prevFrame.translate;
            deltaPos *= g_config.dampenPipboyTranslation; // just use hands dampening value for now
            pipboyScreen->world.translate -= deltaPos;
            _pipboyScreenPrevFrame = pipboyScreen->world;
            f4vr::updateDown(pipboyScreen, false);
        }
    }

    bool Pipboy::isLookingAtPipBoy() const
    {
        const std::string wandPipName("PipboyRoot_NIF_ONLY");
        RE::NiAVObject* pipboy = f4vr::findAVObject(f4vr::getPlayerNodes()->SecondaryWandNode, wandPipName);

        if (pipboy == nullptr) {
            return false;
        }

        const std::string screenName("Screen:0");
        const RE::NiAVObject* screen = f4vr::findAVObject(pipboy, screenName);
        if (screen == nullptr) {
            return false;
        }

        const float threshhold = _pipboyStatus ? g_config.pipboyLookAwayThreshold : g_config.pipboyLookAtThreshold;
        return isCameraLookingAtObject(f4vr::getPlayerCamera()->cameraNode, screen, threshhold);
        return false;
    }

    /**
     * Prevents continuous Input from Right Stick X Axis
     */
    void Pipboy::rightStickXSleep(const int time)
    {
        Sleep(time);
        _controlSleepStickyX = false;
    }

    /**
     * Prevents continuous Input from Right Stick Y Axis
     */
    void Pipboy::rightStickYSleep(const int time)
    {
        Sleep(time);
        _controlSleepStickyY = false;
    }

    /**
     * Used to determine if secondary trigger received a long or short press
     */
    void Pipboy::secondaryTriggerSleep(const int time)
    {
        Sleep(time);
        _controlSleepStickyT = false;
    }
}
