#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>
#include <switch.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdarg>
#include <minizip/zip.h>
#include <minizip/unzip.h>
#include <sys/stat.h>

#include "file.h"
#include "util.h"
#include "ui.h"
#include "gfx.h"
#include "data.h"

#define BUFF_SIZE 0x80000

static std::string wd;

static std::vector<std::string> pathFilter;

static FSFILE *log;
static FSFILE *devuser;

static FsFileSystem sv;

typedef struct
{
    std::string to, from, dev;
    uint64_t *offset;
    zipFile *z;
    bool fin;
} copyArgs;

static copyArgs *copyArgsCreate(const std::string& from, const std::string& to, const std::string& dev, zipFile *z, uint64_t *offset)
{
    copyArgs *ret = new copyArgs;
    ret->to = to;
    ret->from = from;
    ret->dev = dev;
    ret->z = z;
    ret->offset = offset;
    ret->fin = false;
    return ret;
}

static inline void copyArgsDestroy(copyArgs *args)
{
    delete args;
}

static struct
{
    bool operator()(const fs::dirItem& a, const fs::dirItem& b)
    {
        if(a.isDir() != b.isDir())
            return a.isDir();

        for(unsigned i = 0; i < a.getItm().length(); i++)
        {
            char charA = tolower(a.getItm()[i]);
            char charB = tolower(b.getItm()[i]);
            if(charA != charB)
                return charA < charB;
        }
        return false;
    }
} sortDirList;

static void mkdirRec(const std::string& _p)
{
    //skip first slash
    size_t pos = _p.find('/', 0) + 1;
    while((pos = _p.find('/', pos)) != _p.npos)
    {
        mkdir(_p.substr(0, pos).c_str(), 777);
        ++pos;
    }
}

//This is mostly for Pokemon snap, but it also seems to work better? Stops commit errors from journal space
static bool fwriteCommit(const std::string& _path, const void *buf, size_t _size, const std::string& _dev)
{
    size_t written = 0;
    if(data::directFsCmd)
    {
        FSFILE *out = fsfopen(_path.c_str(), FsOpenMode_Write | FsOpenMode_Append);
        written = fsfwrite(buf, 1, _size, out);
        fsfclose(out);
    }
    else
    {
        FILE *out = fopen(_path.c_str(), "ab");
        written = fwrite(buf, 1, _size, out);
        fclose(out);
    }

    Result commit = fsdevCommitDevice(_dev.c_str());

    if(R_FAILED(commit) || written == 0)
        fs::logWrite("Error writing/committing to file \"%s\"\n", _path.c_str());

    return written > 0 && R_SUCCEEDED(commit);
}

void fs::init()
{
    if(fs::fileExists("sdmc:/switch/jksv_dir.txt"))
    {
        char tmp[256];
        FILE *getDir = fopen("sdmc:/switch/jksv_dir.txt", "r");
        fgets(tmp, 256, getDir);
        fclose(getDir);
        wd = tmp;
        util::stripChar('\n', wd);
        util::stripChar('\r', wd);
        mkdirRec(wd);
    }
    else
    {
        mkdir("sdmc:/JKSV", 777);
        wd = "sdmc:/JKSV/";
    }
    fs::logOpen();
}

void fs::exit()
{
    fs::logClose();
}

bool fs::mountSave(const data::user& usr, const data::titledata& open)
{
    Result svOpen;
    switch(open.getType())
    {
        case FsSaveDataType_System:
            svOpen = fsOpen_SystemSaveData(&sv, FsSaveDataSpaceId_System, open.getID(), usr.getUID());
            break;

        case FsSaveDataType_Account:
            svOpen = fsOpen_SaveData(&sv, open.getID(), usr.getUID());
            break;

        case FsSaveDataType_Bcat:
            svOpen = fsOpen_BcatSaveData(&sv, open.getID());
            break;

        case FsSaveDataType_Device:
            svOpen = fsOpen_DeviceSaveData(&sv, open.getID());
            break;

        case FsSaveDataType_Temporary:
            svOpen = fsOpen_TemporaryStorage(&sv);
            break;

        case FsSaveDataType_Cache:
            svOpen = fsOpen_CacheStorage(&sv, open.getID(), open.getSaveIndex());
            break;

        case FsSaveDataType_SystemBcat:
            svOpen = fsOpen_SystemBcatSaveData(&sv, open.getID());
            break;

        default:
            svOpen = 1;
            break;
    }
    return R_SUCCEEDED(svOpen) && fsdevMountDevice("sv", sv) != -1;
}

fs::dirItem::dirItem(const std::string& pathTo, const std::string& sItem)
{
    itm = sItem;

    std::string fullPath = pathTo + sItem;
    struct stat s;
    if(stat(fullPath.c_str(), &s) == 0 && S_ISDIR(s.st_mode))
        dir = true;
}

std::string fs::dirItem::getName() const
{
    size_t extPos = itm.find_last_of('.'), slPos = itm.find_last_of('/');
    if(extPos == itm.npos)
        return "";

    return itm.substr(slPos + 1, extPos);
}

std::string fs::dirItem::getExt() const
{
    size_t extPos = itm.find_last_of('.');
    if(extPos == itm.npos)
        return "";//Folder or no extension
    return itm.substr(extPos + 1, itm.npos);
}

fs::dirList::dirList(const std::string& _path)
{
    path = _path;
    d = opendir(path.c_str());

    while((ent = readdir(d)))
        item.emplace_back(path, ent->d_name);

    closedir(d);

    std::sort(item.begin(), item.end(), sortDirList);
}

void fs::dirList::reassign(const std::string& _path)
{
    path = _path;

    d = opendir(path.c_str());

    item.clear();

    while((ent = readdir(d)))
        item.emplace_back(path, ent->d_name);

    closedir(d);

    std::sort(item.begin(), item.end(), sortDirList);
}

void fs::dirList::rescan()
{
    item.clear();
    d = opendir(path.c_str());

    while((ent = readdir(d)))
        item.emplace_back(path, ent->d_name);

    closedir(d);

    std::sort(item.begin(), item.end(), sortDirList);
}

fs::dataFile::dataFile(const std::string& _path)
{
    f = fopen(_path.c_str(), "r");
    if(f != NULL)
        opened = true;
}

fs::dataFile::~dataFile()
{
    fclose(f);
}

bool fs::dataFile::readNextLine(bool proc)
{
    bool ret = false;
    char tmp[1024];
    while(fgets(tmp, 1024, f))
    {
        if(tmp[0] != '#' && tmp[0] != '\n' && tmp[0] != '\r')
        {
            line = tmp;
            ret = true;
            break;
        }
    }
    util::stripChar('\n', line);
    util::stripChar('\r', line);
    if(proc)
        procLine();

    return ret;
}

void fs::dataFile::procLine()
{
    size_t pPos = line.find_first_of('('), ePos = line.find_first_of('=');
    if(pPos != line.npos || ePos != line.npos)
    {
        lPos = ePos < pPos ? ePos : pPos;
        name.assign(line.begin(), line.begin() + lPos);
    }
    else
        name = "NULL";
    util::stripChar(' ', name);
    ++lPos;
}

std::string fs::dataFile::getNextValueStr()
{
    std::string ret = "";
    //Skip all spaces until we hit actual text
    size_t pos1 = line.find_first_not_of(", ", lPos);
    //If reading from quotes
    if(line[pos1] == '"')
        lPos = line.find_first_of('"', ++pos1);
    else
        lPos = line.find_first_of(",;\n", pos1);//Set lPos to end of string we want. This should just set lPos to the end of the line if it fails, which is ok

    return line.substr(pos1, lPos++ - pos1);
}

int fs::dataFile::getNextValueInt()
{
    int ret = 0;
    std::string no = getNextValueStr();
    if(no[0] == '0' && tolower(no[1]) == 'x')
        ret = strtoul(no.c_str(), NULL, 16);
    else
        ret = strtoul(no.c_str(), NULL, 10);

    return ret;
}

static void copyfile_t(void *a)
{
    copyArgs *args = (copyArgs *)a;

    uint8_t *buff = new uint8_t[BUFF_SIZE];
    if(data::directFsCmd)
    {
        FSFILE *in = fsfopen(args->from.c_str(), FsOpenMode_Read);
        FSFILE *out = fsfopen(args->to.c_str(), FsOpenMode_Write);

        if(!in || !out)
        {
            fsfclose(in);
            fsfclose(out);
            args->fin = true;
            return;
        }

        size_t readIn = 0;
        while((readIn = fsfread(buff, 1, BUFF_SIZE, in)) > 0)
        {
            fsfwrite(buff, 1, readIn, out);
            *args->offset = in->offset;
        }
        fsfclose(in);
        fsfclose(out);
    }
    else
    {
        FILE *in = fopen(args->from.c_str(), "rb");
        FILE *out = fopen(args->to.c_str(), "wb");
        if(!in || !out)
        {
            fclose(in);
            fclose(out);
            args->fin = true;
            return;
        }

        size_t readIn = 0;
        while((readIn = fread(buff, 1, BUFF_SIZE, in)) > 0)
        {
            fwrite(buff, 1, readIn, out);
            *args->offset = ftell(in);
        }
        fclose(in);
        fclose(out);
    }
    delete[] buff;
    args->fin = true;
}

void fs::copyFile(const std::string& from, const std::string& to)
{
    uint64_t progress = 0;
    copyArgs *send = copyArgsCreate(from, to, "", NULL, &progress);

    //Setup progress bar. This thread updates screen, other handles copying
    ui::progBar prog(fsize(from));

    Thread cpyThread;
    threadCreate(&cpyThread, copyfile_t, send, NULL, 0x4000, 0x2B, 1);
    threadStart(&cpyThread);
    while(!send->fin)
    {
        prog.update(progress);
        gfxBeginFrame();
        prog.draw(from, ui::copyHead);
        gfxEndFrame();
    }
    threadClose(&cpyThread);
    copyArgsDestroy(send);
}

void copyFileCommit_t(void *a)
{
    copyArgs *args = (copyArgs *)a;

    uint8_t *buff = new uint8_t[BUFF_SIZE];

    //Create empty destination file using fs
    fsfcreate(args->to.c_str(), 0);

    if(data::directFsCmd)
    {
        FSFILE *in = fsfopen(args->from.c_str(), FsOpenMode_Read);

        if(!in)
        {
            fsfclose(in);
            args->fin = true;
            return;
        }

        size_t readIn = 0;
        while((readIn = fsfread(buff, 1, BUFF_SIZE, in)) > 0)
        {
            if(!fwriteCommit(args->to, buff, readIn, args->dev))
                break;
            *args->offset = in->offset;
        }
        fsfclose(in);
    }
    else
    {
        FILE *in = fopen(args->from.c_str(), "rb");

        if(!in)
        {
            fclose(in);
            args->fin = true;
            return;
        }

        size_t readIn = 0;
        while((readIn = fread(buff, 1, BUFF_SIZE, in)) > 0)
        {
            if(!fwriteCommit(args->to, buff, readIn, args->dev))
                break;

            *args->offset = ftell(in);
        }
        fclose(in);
    }
    delete[] buff;

    Result res = 0;
    if(R_FAILED(res = fsdevCommitDevice(args->dev.c_str())))
        fs::logWrite("Error committing file \"%s\"\n", args->to.c_str());

    args->fin = true;
}

void fs::copyFileCommit(const std::string& from, const std::string& to, const std::string& dev)
{
    uint64_t offset = 0;
    ui::progBar prog(fsize(from));
    copyArgs *send = copyArgsCreate(from, to, dev, NULL, &offset);

    Thread cpyThread;
    threadCreate(&cpyThread, copyFileCommit_t, send, NULL, 0x4000, 0x2B, 1);
    threadStart(&cpyThread);
    while(!send->fin)
    {
        prog.update(offset);
        gfxBeginFrame();
        prog.draw(from, ui::copyHead);
        gfxEndFrame();
    }
    threadClose(&cpyThread);
    copyArgsDestroy(send);
}

void fs::copyDirToDir(const std::string& from, const std::string& to)
{
    dirList list(from);

    for(unsigned i = 0; i < list.getCount(); i++)
    {
        if(pathIsFiltered(from + list.getItem(i)))
            continue;

        if(list.isDir(i))
        {
            std::string newFrom = from + list.getItem(i) + "/";
            std::string newTo   = to + list.getItem(i) + "/";
            mkdir(newTo.substr(0, newTo.length() - 1).c_str(), 0777);

            copyDirToDir(newFrom, newTo);
        }
        else
        {
            std::string fullFrom = from + list.getItem(i);
            std::string fullTo   = to   + list.getItem(i);
            copyFile(fullFrom, fullTo);
        }
    }
}

void copyFileToZip_t(void *a)
{
    copyArgs *args = (copyArgs *)a;
    FILE *cpy = fopen(args->from.c_str(), "rb");

    size_t readIn = 0;
    uint8_t *inBuff= new uint8_t[BUFF_SIZE];
    while((readIn = fread(inBuff, 1, BUFF_SIZE, cpy)) > 0)
    {
        if(zipWriteInFileInZip(*args->z, inBuff, readIn) != 0)
        {
            fs::logWrite("zipWriteInFileInZip failed -> \"%s\"\n", args->from.c_str());
            break;
        }

        *args->offset = ftell(cpy);
    }

    delete[] inBuff;
    fclose(cpy);
    args->fin = true;
}

void copyFileToZip(const std::string& from, zipFile *z)
{
    ui::progBar prog(fs::fsize(from));
    uint64_t progress = 0;
    copyArgs *send = copyArgsCreate(from, "", "", z, &progress);

    Thread cpyThread;
    threadCreate(&cpyThread, copyFileToZip_t, send, NULL, 0x4000, 0x2B, 1);
    threadStart(&cpyThread);
    while(!send->fin)
    {
        prog.update(progress);
        gfxBeginFrame();
        prog.draw(from, ui::copyHead);
        gfxEndFrame();
    }
    threadClose(&cpyThread);
    copyArgsDestroy(send);
}

void fs::copyDirToZip(const std::string& from, zipFile *to)
{
    fs::dirList list(from);

    for(unsigned i = 0; i < list.getCount(); i++)
    {
        if(pathIsFiltered(from + list.getItem(i)))
            continue;

        if(list.isDir(i))
        {
            std::string newFrom = from + list.getItem(i) + "/";
            fs::copyDirToZip(newFrom, to);
        }
        else
        {
            zip_fileinfo inf = { 0 };
            std::string filename = from + list.getItem(i);
            size_t devPos = filename.find_first_of('/') + 1;
            if(zipOpenNewFileInZip(*to, filename.substr(devPos, filename.length()).c_str(), &inf, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION) == ZIP_OK)
                copyFileToZip(std::string(from) + list.getItem(i).c_str(), to);
            zipCloseFileInZip(*to);
        }
    }
}

void fs::copyZipToDir(unzFile *unz, const std::string& to, const std::string& dev)
{
    char filename[FS_MAX_PATH];
    uint8_t *buff = new uint8_t[BUFF_SIZE];
    int readIn = 0;
    unz_file_info info;
    do
    {
        unzGetCurrentFileInfo(*unz, &info, filename, FS_MAX_PATH, NULL, 0, NULL, 0);
        if(unzOpenCurrentFile(*unz) == UNZ_OK)
        {
            std::string path = to + filename;
            mkdirRec(path.substr(0, path.find_last_of('/') + 1));
            ui::progBar prog(info.uncompressed_size);
            size_t done = 0;

            //Create new empty file using FS
            fsfcreate(path.c_str(), 0);

            if(data::directFsCmd)
            {
                while((readIn = unzReadCurrentFile(*unz, buff, BUFF_SIZE)) > 0)
                {
                    done += readIn;
                    fwriteCommit(path, buff, readIn, dev);
                    prog.update(done);

                    gfxBeginFrame();
                    prog.draw(filename, ui::copyHead);
                    gfxEndFrame();
                }
            }
            else
            {
                while((readIn = unzReadCurrentFile(*unz, buff, BUFF_SIZE)) > 0)
                {
                    done += readIn;
                    fwriteCommit(path, buff, readIn, dev);
                    prog.update(done);

                    gfxBeginFrame();
                    prog.draw(filename, ui::copyHead);
                    gfxEndFrame();
                }
            }
            unzCloseCurrentFile(*unz);
            if(R_FAILED(fsdevCommitDevice(dev.c_str())))
                ui::showMessage("*Error*", "Error committing file to device.");
        }
    }
    while(unzGoToNextFile(*unz) != UNZ_END_OF_LIST_OF_FILE);

    delete[] buff;
}

void fs::copyDirToDirCommit(const std::string& from, const std::string& to, const std::string& dev)
{
    dirList list(from);

    for(unsigned i = 0; i < list.getCount(); i++)
    {
        if(list.isDir(i))
        {
            std::string newFrom = from + list.getItem(i) + "/";
            std::string newTo   = to + list.getItem(i);
            mkdir(newTo.c_str(), 0777);
            newTo += "/";

            copyDirToDirCommit(newFrom, newTo, dev);
        }
        else
        {
            std::string fullFrom = from + list.getItem(i);
            std::string fullTo   = to   + list.getItem(i);
            copyFileCommit(fullFrom, fullTo, dev);
        }
    }
}

void fs::delfile(const std::string& path)
{
    if(data::directFsCmd)
        fsremove(path.c_str());
    else
        remove(path.c_str());
}

void fs::delDir(const std::string& path)
{
    dirList list(path);
    for(unsigned i = 0; i < list.getCount(); i++)
    {
        if(pathIsFiltered(path + list.getItem(i)))
            continue;

        if(list.isDir(i))
        {
            std::string newPath = path + list.getItem(i) + "/";
            delDir(newPath);

            std::string delPath = path + list.getItem(i);
            rmdir(delPath.c_str());
        }
        else
        {
            std::string delPath = path + list.getItem(i);
            std::remove(delPath.c_str());
        }
    }
    rmdir(path.c_str());
}

void fs::loadPathFilters(const std::string& _file)
{
    if(fs::fileExists(_file))
    {
        fs::dataFile filter(_file);
        while(filter.readNextLine(false))
            pathFilter.push_back(filter.getLine());
    }
}

bool fs::pathIsFiltered(const std::string& _path)
{
    if(pathFilter.empty())
        return false;

    for(std::string& _p : pathFilter)
    {
        if(_path == _p)
            return true;
    }

    return false;
}

void fs::freePathFilters()
{
    pathFilter.clear();
}

bool fs::dumpAllUserSaves(const data::user& u)
{
    for(unsigned i = 0; i < u.titles.size(); i++)
    {
        ui::updatePad();

        if(ui::padKeysDown() & HidNpadButton_B)
            return false;

        if(fs::mountSave(u, u.titles[i]))
        {
            u.titles[i].createDir();
            std::string filtersPath = u.titles[i].getPath() + "pathFilters.txt";
            fs::loadPathFilters(filtersPath);
            switch(data::zip)
            {
                case true:
                    {
                        std::string outPath = u.titles[i].getPath() + u.getUsernameSafe() + " - " + util::getDateTime(util::DATE_FMT_YMD) + ".zip";
                        zipFile zip = zipOpen(outPath.c_str(), 0);
                        fs::copyDirToZip("sv:/", &zip);
                        zipClose(zip, NULL);
                    }
                    break;

                case false:
                    {
                        std::string outPath = u.titles[i].getPath() + u.getUsernameSafe() + " - " + util::getDateTime(util::DATE_FMT_YMD) + "/";
                        mkdir(outPath.substr(0, outPath.length() - 1).c_str(), 777);
                        fs::copyDirToDir("sv:/", outPath);
                    }
                    break;
            }
            fsdevUnmountDevice("sv");
            fs::freePathFilters();
        }
    }
    return true;//?
}

std::string fs::getFileProps(const std::string& _path)
{
    std::string ret = "";

    FILE *get = fopen(_path.c_str(), "rb");
    if(get != NULL)
    {
        //Size
        fseek(get, 0, SEEK_END);
        unsigned fileSize = ftell(get);
        fseek(get, 0, SEEK_SET);

        fclose(get);

        //Probably add more later

        char tmp[256];
        std::sprintf(tmp, "Path: #%s#\nSize: %u", _path.c_str(), fileSize);

        ret = tmp;
    }
    return ret;
}

void fs::getDirProps(const std::string& _path, uint32_t& dirCount, uint32_t& fileCount, uint64_t& totalSize)
{
    fs::dirList list(_path);

    for(unsigned i = 0; i < list.getCount(); i++)
    {
        if(list.isDir(i))
        {
            ++dirCount;
            std::string newPath = _path + list.getItem(i) + "/";
            uint32_t dirAdd = 0, fileAdd = 0;
            uint64_t sizeAdd = 0;

            getDirProps(newPath, dirAdd, fileAdd, sizeAdd);
            dirCount += dirAdd;
            fileCount += fileAdd;
            totalSize += sizeAdd;
        }
        else
        {
            ++fileCount;
            std::string filePath = _path + list.getItem(i);

            FILE *gSize = fopen(filePath.c_str(), "rb");
            fseek(gSize, 0, SEEK_END);
            size_t fSize = ftell(gSize);
            fclose(gSize);

            totalSize += fSize;
        }
    }
}

bool fs::fileExists(const std::string& path)
{
    FILE *test = fopen(path.c_str(), "rb");
    if(test != NULL)
    {
        fclose(test);
        return true;
    }

    return false;
}

size_t fs::fsize(const std::string& _f)
{
    size_t ret = 0;
    FILE *get = fopen(_f.c_str(), "rb");
    if(get != NULL)
    {
        fseek(get, 0, SEEK_END);
        ret = ftell(get);
    }
    fclose(get);
    return ret;
}

std::string fs::getWorkDir() { return wd; }

bool fs::isDir(const std::string& _path)
{
    struct stat s;
    return stat(_path.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
}

void fs::logOpen()
{
    std::string logPath = wd + "log.txt";
    remove(logPath.c_str());
    log = fsfopen(logPath.c_str(), FsOpenMode_Write);
}

void fs::logWrite(const char *fmt, ...)
{
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    vsprintf(tmp, fmt, args);
    va_end(args);
    fsfwrite(tmp, 1, strlen(tmp), log);
}

void fs::logClose()
{
    fsfclose(log);
}

void fs::writeDevuserFile(std::string path, std::string name) {
    std::string fulldevpath = path + "/activeDevUser.txt";
    devuser = fsfopen(fulldevpath.c_str(), FsOpenMode_Write);
    fsfwrite(name.c_str(), 1, strlen(name.c_str()), devuser);
    fsfclose(devuser);
}

std::string fs::readDevuserFile(std::string path) {
    std::string fulldevpath = path + "/activeDevUser.txt";
    devuser = fsfopen(fulldevpath.c_str(), FsOpenMode_Read);
    if (devuser) {
        char name[200];
        fsfread(name, 1, 200, devuser);
        fsfclose(devuser);
        return name;
    } else {
        return "";
    }
}

