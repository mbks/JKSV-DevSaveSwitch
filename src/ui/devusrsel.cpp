#include <string>
#include <vector>
#include <switch.h>

#include "data.h"
#include "ui.h"
#include "uiupdate.h"
#include "file.h"
#include "util.h"

void ui::drawDevUserMenu()
{
    //Static so they don't get reset every loop
    static int start = 0;

    static uint8_t clrShft = 0;
    static bool clrAdd = true;

    static unsigned selRectX = 276, selRectY = 160;

    if(clrAdd)
    {
        clrShft += 6;
        if(clrShft >= 0x72)
            clrAdd = false;
    }
    else
    {
        clrShft -= 3;
        if(clrShft <= 0)
            clrAdd = true;
    }

    unsigned x = 280, y = 164;
    unsigned endUser = data::users.size()-1-4; // TODO replace hardcoded 4

    drawBoundBox(selRectX, selRectY, 140, 140, clrShft);

    for(unsigned i = start; i < endUser; y += 136)
    {
        unsigned endRow = i + 5;
        for(unsigned tX = x; i < endRow; i++, tX += 144)
        {
            if(i == endUser)
                break;

            if((int)i == data::selUser)
            {
                selRectX = tX - 6;
                selRectY = y - 6;

                unsigned userWidth = textGetWidth(data::curUser.getUsername().c_str(), ui::shared, 18);
                int userRectWidth = userWidth + 32, userRectX = (tX + 64) - (userRectWidth  / 2);
                if(userRectX < 16)
                    userRectX = 16;
                else if(userRectX + userRectWidth > 1264)
                    userRectX = 1264 - userRectWidth;

                drawTextbox(frameBuffer, userRectX, y - 50, userRectWidth, 38);
                drawText(data::curUser.getUsername().c_str(), frameBuffer, ui::shared, userRectX + 16, y - 40, 18, ui::txtDiag);
            }
            data::users[i].drawIconHalf(tX, y);
        }
    }
}

void ui::updateDevUserMenu(const uint64_t& down, const uint64_t& held)
{
    std::string name;
    std::string curDevUser;
    std::string fromPath;
    struct stat statbuf;
    int prev_user;
    switch(down)
    {
        case HidNpadButton_A:
            prev_user = data::selUser;
            name = data::curUser.getUsernameSafe();
            data::selUser = data::devSwitchUser; // this needs to be here, otherwise loaded path is wrong
            curDevUser = fs::readDevuserFile(data::curData.getPath());

            // folder of user that requested this action
            fromPath = data::curData.getPath() + "/DevSaveSwitchAuto-" + name + "/";
            stat(fromPath.c_str(), &statbuf);

            if (curDevUser == name) { // user save already active, nothing to do
                std::string text = "Device save of ";
                text.append(name.c_str());
                text.append(" already active.");
                ui::showPopup(POP_FRAME_DEFAULT, text.c_str());
            } else {
                if(fs::mountSave(data::curUser, data::curData)) {
                    data::curData.createDir();
                    fs::writeDevuserFile(data::curData.getPath(), name);

                    if (curDevUser != "") { // backup save of active user unless non-existent
                        std::string path = data::curData.getPath() + "/DevSaveSwitchAuto-" + curDevUser;
                        mkdir(path.c_str(), 777);
                        path += "/";
                        fs::copyDirToDir("sv:/", path);
                    }
                    if (!S_ISDIR(statbuf.st_mode)) { // selected user has no save to restore fom
                        mkdir(fromPath.c_str(), 777);
                        fs::writeDevuserFile(data::curData.getPath(), name);
                        ui::showPopup(POP_FRAME_DEFAULT, "New folder was created, please RESTORE a backup of this user now!");
                    } else {
                        fs::wipeSave();
                        fs::copyDirToDirCommit(fromPath, "sv:/", "sv");
                        fs::unmountSave();
                        std::string text = "Device save of ";
                        text.append(name);
                        text.append(" is now active!");
                        ui::showPopup(POP_FRAME_DEFAULT, text.c_str());
                    }
                    fs::unmountSave();
                }
            }
            data::selUser = prev_user;



            /*
            if(fs::mountSave(data::curUser, data::curData))
            {
                folderMenuPrepare(data::curUser, data::curData);
                ui::folderMenuInfo = util::getInfoString(data::curUser, data::curData);
                ui::changeState(FLD_SEL);
            }*/
            break;

        case HidNpadButton_B:
            data::selUser = data::devSwitchUser;
            ui::changeState(TTL_SEL);
            break;

        case HidNpadButton_StickLUp:
        case HidNpadButton_Up:
            data::selUser - 5 < 0 ? data::selUser = 0 : data::selUser -= 4;
            break;

        case HidNpadButton_StickLDown:
        case HidNpadButton_Down:
            data::selUser + 5 > (int)data::users.size() - 1 ? data::selUser = data::users.size() - 1 : data::selUser += 5;
            break;

        case HidNpadButton_StickLLeft:
        case HidNpadButton_Left:
            if(data::selUser > 0)
                --data::selUser;
            break;

        case HidNpadButton_StickLRight:
        case HidNpadButton_Right:
            if(data::selUser < (int)data::users.size() - 1 - 4)
                ++data::selUser;
            break;
    }
}
