

#include <sstream>

#include "payload.h"
#include "payloadHandler.h"

using namespace fastCFWSwitcher;

#define IRAM_PAYLOAD_MAX_SIZE 0x2F000
#define IRAM_PAYLOAD_BASE 0x40010000


alignas(0x1000) static u8 g_ff_page[0x1000];
alignas(0x1000) static u8 g_work_page[0x1000];
alignas(0x1000) static  u8 g_reboot_payload[IRAM_PAYLOAD_MAX_SIZE];


//Hekate config and magic
#define HEKATE_AUTOBOOT_POS 0x94
#define HEKATE_MAGIC_POS 0x118
#define HEKATE_VERSION 0x11C
#define HEKATE_MAGIC 0x43544349

#define BOOT_CFG_AUTOBOOT_EN (1 << 0)
#define BOOT_CFG_FROM_LAUNCH (1 << 1)
#define BOOT_CFG_FROM_ID     (1 << 2)
#define BOOT_CFG_TO_EMUMMC   (1 << 3)
#define BOOT_CFG_SEPT_RUN    (1 << 7)


typedef struct __attribute__((__packed__)) _boot_cfg_t
{
    u8 boot_cfg;
    u8 autoboot;
    u8 autoboot_list;
    u8 extra_cfg;
    union
    {
        struct
        {
            char id[8]; // 7 char ASCII null teminated.
            char emummc_path[0x78]; // emuMMC/XXX, ASCII null teminated.
        };
        u8 ums; // nyx_ums_type.
        u8 xt_str[0x80];
    };
} boot_cfg_t;

void PayloadHandler::do_iram_dram_copy(void *buf, uintptr_t iram_addr, size_t size, int option) {
    memcpy(g_work_page, buf, size);
    
    SecmonArgs args = {0};
    args.X[0] = 0xF0000201;             /* smcAmsIramCopy */
    args.X[1] = (uintptr_t)g_work_page;  /* DRAM Address */
    args.X[2] = iram_addr;              /* IRAM Address */
    args.X[3] = size;                   /* Copy size */
    args.X[4] = option;                 /* 0 = Read, 1 = Write */
    svcCallSecureMonitor(&args);
    
    memcpy(buf, g_work_page, size);
}

void PayloadHandler::copy_to_iram(uintptr_t iram_addr, void *buf, size_t size) {
    do_iram_dram_copy(buf, iram_addr, size, 1);
}
void PayloadHandler::copy_from_iram(void *buf, uintptr_t iram_addr, size_t size) {
    do_iram_dram_copy(buf, iram_addr, size, 0);
}


void PayloadHandler::clear_iram(void) {
    memset(g_ff_page, 0xFF, sizeof(g_ff_page));
    for (size_t i = 0; i < IRAM_PAYLOAD_MAX_SIZE; i += sizeof(g_ff_page)) {
        copy_to_iram(IRAM_PAYLOAD_BASE + i, g_ff_page, sizeof(g_ff_page));
    }
}

void PayloadHandler::setError(std::string errorString){
    this->frame->setSubtitle(errorString);
}

void PayloadHandler::applyPayloadArgs(fastCFWSwitcher::Payload* payload){
    PayloadType payloadType = getBinPayloadType(payload);

    switch(payloadType){
        case PayloadType::HEKATE:
            int hekateVersion = strtol((char*)&g_reboot_payload[HEKATE_VERSION], (char **)NULL, 10);
            if(hekateVersion>=502 && !payload->getBootId().empty()){
                boot_cfg_t* hekateCFG = (boot_cfg_t*) &g_reboot_payload[HEKATE_AUTOBOOT_POS];
                hekateCFG->boot_cfg = BOOT_CFG_FROM_ID|BOOT_CFG_AUTOBOOT_EN;

                std::string bootID = payload->getBootId();
                strcpy(hekateCFG->id, bootID.c_str());

            } else if (payload->getBootPos()!=-1){
                boot_cfg_t* hekateCFG = (boot_cfg_t*) &g_reboot_payload[HEKATE_AUTOBOOT_POS];
                hekateCFG->boot_cfg = BOOT_CFG_AUTOBOOT_EN;
                hekateCFG->autoboot = payload->getBootPos();
                hekateCFG->autoboot_list = 0;
            }
    }
}


PayloadType PayloadHandler::getBinPayloadType(fastCFWSwitcher::Payload* payload){

    u32 hekateMagic=*((u32*)&g_reboot_payload[HEKATE_MAGIC_POS]);
    if(hekateMagic==HEKATE_MAGIC){
        return PayloadType::HEKATE;

    } 
    return PayloadType::UNKOWN;
}


bool PayloadHandler::loadPayload(fastCFWSwitcher::Payload* payload){
    FsFileSystem fsSdmc;
    FsFile fileConfig;

    if(R_FAILED(fsOpenSdCardFileSystem(&fsSdmc))){
        setError("open sd failed\n");
        return false;
    }

    /* Open config file. */
    char pathBuf[FS_MAX_PATH]; 
    strcpy(pathBuf, payload->getPath().c_str());//fixes error 0xd401
    Result fsOpenResult = fsFsOpenFile(&fsSdmc, pathBuf, FsOpenMode_Read, &fileConfig);
    if (R_FAILED(fsOpenResult)){
        setError("open file failed"+std::to_string(fsOpenResult));
        fsFsClose(&fsSdmc);
        return false;
    }


    /* Get config file size. */
    s64 configFileSize;
    if (R_FAILED(fsFileGetSize(&fileConfig, &configFileSize))){
        setError("get file size failed\n");
        fsFileClose(&fileConfig);
        fsFsClose(&fsSdmc);
        return false;
    }

    if(configFileSize>sizeof(g_reboot_payload)){
        setError("to big\n");
        fsFileClose(&fileConfig);
        fsFsClose(&fsSdmc);
        return false;
    }

    memset(g_reboot_payload, 0xFF, sizeof(g_reboot_payload));
    u64 readSize;
    Result rc = fsFileRead(&fileConfig, 0, g_reboot_payload, configFileSize, FsReadOption_None, &readSize);
    if (R_FAILED(rc) || readSize != static_cast<u64>(configFileSize)){
        setError("read file failed\n");
        fsFileClose(&fileConfig);
        fsFsClose(&fsSdmc);
        return false;
    }

    fsFileClose(&fileConfig);
    fsFsClose(&fsSdmc);
    return true;
}

//todo better error handling
void PayloadHandler::rebootToPayload(fastCFWSwitcher::Payload* payload) {
    smInitialize();
    Result splResult = splInitialize();
    smExit();
    if (R_FAILED(splResult)){
        std::stringstream ss;
        ss<< std::hex << splResult; // int decimal_value
        std::string res ( ss.str() );
        setError("splInitFailed: "+res);
        return ;
    }

    bool loadResult = loadPayload(payload);

    if(!loadResult){
        splExit();
        return;
    }

    applyPayloadArgs(payload);

    clear_iram();
    for (size_t i = 0; i < IRAM_PAYLOAD_MAX_SIZE; i += 0x1000) {
        copy_to_iram(IRAM_PAYLOAD_BASE + i, &g_reboot_payload[i], 0x1000);
    }

    //check for corectness
    int result=1;
    for (size_t i = 0; i < IRAM_PAYLOAD_MAX_SIZE; i += 0x1000) {
        copy_from_iram(&g_ff_page, IRAM_PAYLOAD_BASE + i, 0x1000);
        result = memcmp(&g_ff_page,&g_reboot_payload[i], 0x1000);
        if(result){
            result=i;
            break;
        }
    }
    if(result){
        setError("cmp failed"+std::to_string(result));
    } else{
        splSetConfig((SplConfigItem)65001, 2);
    }

    splExit();
}
