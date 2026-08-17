#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <timeapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <shlobj.h>
#include "mp_shared.h"
#include "zstd.h"
extern "C" {
#include "bspatch.h"
}

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"iphlpapi.lib")
#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")

#define MSG_SAVE          1
#define MSG_PING          2
#define MSG_PONG          3
#define MSG_CHAT          4
#define MSG_SAVE_FULL     5
#define MSG_SAVE_DIFF     6
#define MSG_BUILD         7
#define MSG_RESOURCE_REQ  8
#define MSG_RESOURCE_RESP 9
#define MSG_PLAYER_LIST   10
#define MSG_TERRITORY     11
#define MSG_DEMOLISH      12
#define MSG_ADMIN         13

#define TARGET_FPS          60
#define FRAME_MS            (1000/TARGET_FPS)
#define FRAME_MS_UNFOCUSED  (1000/30)
#define POLL_ONLINE_MS      150
#define POLL_OFFLINE_MS     500
#define MAX_CHAT            300
#define MAX_CONN_LOG        100
#define MAX_SAVED_SERVERS   8
#define PING_HISTORY        90
#define ADMIN_KICK          1
#define ADMIN_MUTE          2
#define ADMIN_UNMUTE        3
#define ADMIN_BROADCAST     4

struct MsgHeader  { BYTE type; DWORD size; };

struct BuildCmd {
    float x, z, rotation;
    char  typeName[128];
    char  playerName[64];
};

struct DemolishCmd {
    float x, z;
    char  playerName[64];
    DWORD seq;
};

struct ResourceReq {
    char fromPlayer[64];
    char toPlayer[64];
    char resource[64];
    int  amount;
    int  price;
    BYTE accepted;
};

struct TerritoryPacket {
    char  playerName[64];
    float x1, z1, x2, z2;
    BYTE  action;
    DWORD color;
};

struct AdminPacket {
    BYTE command;
    char target[64];
    char param[128];
};

struct PatchStream { const char* buf; size_t pos; size_t size; };

enum ChatType {
    CHAT_NORMAL, CHAT_SYSTEM, CHAT_BUILD,
    CHAT_TRADE,  CHAT_ERROR,  CHAT_DEMO
};

struct ChatMsg {
    std::string text;
    ChatType    type;
    double      ts;
};

struct PlayerInfo {
    std::string name;
    DWORD       ping;
    bool        connected;
    ImVec4      color;
    int         builds;
    int         demolishes;
    float       tx1, tz1, tx2, tz2;
    bool        hasTerr;
    DWORD       terrColor;
};

struct BuildLog {
    std::string player;
    std::string type;
    float x, z;
    double ts;
};

struct DemolLog {
    std::string player;
    float x, z;
    double ts;
};

struct ResourceDeal {
    std::string from, to, resource;
    int  amount, price;
    bool pending, accepted;
    double ts;
};

struct Notification {
    std::string text;
    ImVec4      color;
    double      ts;
};

struct ConnLogEntry {
    std::string text;
    ImVec4      color;
    double      ts;
};

struct SavedServer {
    char ip[64];
    char port[8];
    char name[64];
    char lastSeen[32];
};

enum Language { LANG_EN = 0, LANG_RU = 1 };
static Language g_lang = LANG_EN;

struct Strings {
    const char* windowTitle;
    const char* tabMain, *tabImport, *tabBuildings, *tabMap, *tabStats, *tabAdmin, *tabSettings;
    const char* pluginLabel, *networkLabel;
    const char* gameRunning, *waitingGame;
    const char* modeLabel, *modeHost, *modeClient;
    const char* gameLabel, *gameCoop, *gameTerr;
    const char* btnStartHost, *btnConnect, *btnDisconnect;
    const char* yourZeroTier, *portSuffix;
    const char* playersOnline, *noPlayers;
    const char* recentBuilds, *noBuildsYet;
    const char* sendBtn;
    const char* statusOnline, *statusStandby, *statusOffline;
    const char* sessionHdr, *activityHdr, *playerStatsHdr, *connLogHdr;
    const char* noPlayersConn;
    const char* pluginRow, *networkRow, *durationRow, *pingRow, *modeRow, *zerotierRow;
    const char* coopStr, *terrStr;
    const char* buildingsRow, *chatRow, *dealsRow, *playersRow, *demolRow;
    const char* totalPlaced, *totalDemol, *mostActive, *filterLabel;
    const char* buildHistHdr, *demolHistHdr, *noBuildingsYet, *noDemolYet;
    const char* categoryHdr, *resourceHdr, *sendReqHdr;
    const char* resourceLabel, *amountLabel, *priceLabel, *priceFree, *sendReqBtn;
    const char* dealsHdr, *noDeals;
    const char* fromCol, *toCol, *allStr, *statusCol;
    const char* pendingStr, *acceptedStr, *declinedStr;
    const char* langHdr;
    const char* playerCol, *buildingCol, *whenCol;
    const char* adminHdr, *noAdminConn;
    const char* adminSelectPlayer, *adminReasonLabel, *adminMsgLabel;
    const char* adminKick, *adminMute, *adminUnmute, *adminBroadcast;
    const char* terrHdr, *terrSetBtn, *terrClearBtn;
    const char* settingsHdr, *nameLabel, *portLabel;
    const char* savedServersHdr, *noSavedServers, *connectBtn, *deleteBtn;
    const char* showBuildNotif, *autoscrollChat, *showDemoOnMap, *showTerr;
    const char* pingGraphHdr, *clearBtn, *copyBtn;
    const char* selectResource;
};

static const Strings g_str[2] = {
{
    "WRSR Multiplayer  v0.8",
    "Main","Import","Buildings","Map","Stats","Admin","Settings",
    "Plugin:","Network:",
    "Game running","Waiting for game...",
    "Mode:","Host","Client",
    "Game:","Coop","Terr",
    "Start Hosting","Connect","Disconnect",
    "Your ZeroTier IP: "," port: 7777",
    "Players Online","  No players",
    "Recent Builds","  No builds yet",
    "Send",
    "ONLINE","STANDBY","OFFLINE",
    "Session","Activity","Player Stats","Connection Log",
    "  No players connected",
    "Plugin:   ","Network:  ","Duration: ","Ping:     ","Mode:     ","ZeroTier: ",
    "Cooperative","Territories",
    "Builds:    ","Chat:      ","Deals:     ","Players:   ","Demolish:  ",
    "Total placed:","Total demolished:","Most active:","Filter:",
    "Build History","Demolish History","  No buildings yet","  No demolitions yet",
    "Category","Resource","Send Request",
    "Resource: ","Amount","Price","(0=free)","Send Request",
    "Resource Deals","  No deals yet",
    "From","To","all","Status","Pending","Accepted","Declined",
    "Language",
    "Player","Building","When",
    "Admin Panel","  Connect to server first",
    "Select player:","Reason:","Message:",
    "Kick","Mute","Unmute","Broadcast",
    "Territories","Set Territory","Clear Territory",
    "Settings","Name:","Port:",
    "Saved Servers","  No saved servers","Connect","Delete",
    "Build notifications","Auto-scroll chat","Show demolitions on map","Show territories",
    "Ping History","Clear","Copy",
    "(select resource)"
},
{
    "WRSR Мультиплеер  v0.8",
    "Главная","Импорт","Здания","Карта","Статы","Админ","Настройки",
    "Плагин:","Сеть:",
    "Игра запущена","Ожидание игры...",
    "Режим:","Хост","Клиент",
    "Игра:","Коп","Терр",
    "Начать хостинг","Подключиться","Отключиться",
    "Твой ZeroTier IP: "," порт: 7777",
    "Игроки онлайн","  Нет игроков",
    "Последние постройки","  Нет построек",
    "Отпр",
    "ОНЛАЙН","ОЖИДАНИЕ","ОФЛАЙН",
    "Сессия","Активность","Статы игроков","Лог подключения",
    "  Нет подключённых игроков",
    "Плагин:   ","Сеть:     ","Длит.:    ","Пинг:     ","Режим:    ","ZeroTier: ",
    "Кооператив","Территории",
    "Постройки: ","Чат:       ","Сделки:    ","Игроки:    ","Сносы:     ",
    "Всего поставлено:","Всего снесено:","Самый активный:","Фильтр:",
    "История построек","История сносов","  Нет построек","  Нет сносов",
    "Категория","Ресурс","Запрос ресурса",
    "Ресурс: ","Кол-во","Цена","(0=бесплатно)","Запросить",
    "Сделки с ресурсами","  Нет сделок",
    "От","Кому","все","Статус","Ожидает","Принято","Отклонено",
    "Язык",
    "Игрок","Здание","Когда",
    "Админ панель","  Сначала подключись к серверу",
    "Выбери игрока:","Причина:","Сообщение:",
    "Кик","Мут","Размут","Рассылка",
    "Территории","Установить","Очистить",
    "Настройки","Имя:","Порт:",
    "Сохранённые серверы","  Нет сохранённых серверов","Подключиться","Удалить",
    "Уведомления о постройках","Автопрокрутка чата","Показывать сносы на карте","Показывать территории",
    "График пинга","Очистить","Копировать",
    "(выбери ресурс)"
}
};

static const Strings& L() { return g_str[g_lang]; }

static void GetIniPath(char* out, int sz)
{
    GetModuleFileNameA(NULL, out, sz);
    char* last = strrchr(out, '\\');
    if (last) { last[1] = 0; strcat(out, "mp_overlay.ini"); }
}

static void SaveLanguage()
{
    char path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    char v[4]; snprintf(v, 4, "%d", (int)g_lang);
    WritePrivateProfileStringA("overlay","language",v,path);
}

static void LoadLanguage()
{
    char path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    int v = GetPrivateProfileIntA("overlay","language",0,path);
    g_lang = (v==1) ? LANG_RU : LANG_EN;
}

static void ResetLanguageToEN()
{
    char path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    WritePrivateProfileStringA("overlay","language","0",path);
    g_lang = LANG_EN;
}

static void SaveSettings(const char* name, const char* ip, const char* port)
{
    char path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    WritePrivateProfileStringA("settings","name",name,path);
    WritePrivateProfileStringA("settings","ip",ip,path);
    WritePrivateProfileStringA("settings","port",port,path);
}

static void LoadSettings(char* name, char* ip, char* port)
{
    char path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    GetPrivateProfileStringA("settings","name","Player1",name,64,path);
    GetPrivateProfileStringA("settings","ip","127.0.0.1",ip,64,path);
    GetPrivateProfileStringA("settings","port","7777",port,8,path);
}

struct ResCat { const char* name; const char* kw[20]; };
static ResCat g_cats[] = {
    {"Raw Materials",  {"rawcoal","rawgravel","rawiron","rawbauxite","rawgold","rawcopper","rawuranium",NULL}},
    {"Fuel & Energy",  {"coal","oil","fuel","nuclearfuel","nuclearfuelburned","yellowcake","heat","eletric","gas",NULL}},
    {"Construction",   {"bricks","boards","cement","steel","prefabpanels","asphalt","gravel","wood","concrete","sand","glass",NULL}},
    {"Industry",       {"iron","aluminium","alumina","bauxite","plastics","chemicals","explosives","eletronics","fabric","rubber","bitumen",NULL}},
    {"Agriculture",    {"food","meat","livestock","fertiliser","fertiliser_liquid","plants","alcohol",NULL}},
    {"Liquids",        {"water","colorwater","usagewater",NULL}},
    {"Waste",          {"waste",NULL}},
    {"Cargo",          {"container_big","container_small","crate","generalcargo",NULL}},
    {"Nuclear",        {"uranium","yellowcake","nuclearfuel","nuclearfuelburned",NULL}},
    {"Other",          {NULL}},
};
static const int g_catCount = 10;

static ImVec4 g_playerColors[4] = {
    ImVec4(0.90f,0.30f,0.20f,1.f),
    ImVec4(0.20f,0.60f,0.90f,1.f),
    ImVec4(0.20f,0.80f,0.35f,1.f),
    ImVec4(0.90f,0.70f,0.10f,1.f),
};

static ImU32 g_playerColorsU32[4] = {
    IM_COL32(230,75,50,220),
    IM_COL32(50,140,220,220),
    IM_COL32(50,200,80,220),
    IM_COL32(220,180,50,220),
};

static SharedMemory g_shm;
static bool         g_pluginFound      = false;
static bool         g_networkOnline    = false;
static BYTE         g_pluginStatus     = MP_STATUS_OFFLINE;
static char         g_pluginStatusText[256] = "Waiting for game...";
static char         g_myZeroTierIP[64] = "";
static bool         g_userDisconnected = false;
static DWORD        g_shmUptime        = 0;
static DWORD        g_shmBuilds        = 0;

struct AppState {
    char ip[64]   = "127.0.0.1";
    char port[8]  = "7777";
    char name[64] = "Player1";
    bool isHost      = true;
    bool isCoopMode  = true;

    SOCKET           sock        = INVALID_SOCKET;
    HANDLE           recvThread  = NULL;
    CRITICAL_SECTION sendLock;
    ULONGLONG        lastPingTime = 0;
    DWORD            ping         = 0;
    bool             connected    = false;

    int  progressCur = 0;
    int  progressMax = 0;
    bool syncing     = false;
    std::string syncStatus;

    std::vector<PlayerInfo>   players;
    std::vector<ChatMsg>      chat;
    std::vector<BuildLog>     builds;
    std::vector<DemolLog>     demols;
    std::vector<ResourceDeal> deals;
    std::vector<Notification> notifications;
    std::vector<ConnLogEntry> connLog;
    std::vector<std::string>  allResources;
    std::vector<std::string>  catResources;
    SavedServer               savedServers[MAX_SAVED_SERVERS];
    int                       savedServerCount = 0;

    std::vector<float> pingHistory;

    char  chatInput[256]  = {};
    char  buildFilter[64] = {};
    char  demolFilter[64] = {};
    char  resFilter[64]   = {};
    char  adminTarget[64] = {};
    char  adminReason[128]= {};
    char  adminMsg[256]   = {};
    int   selectedCat     = 0;
    int   selectedRes     = -1;
    char  amountBuf[16]   = "100";
    char  priceBuf[16]    = "0";
    float mapZoom   = 1.0f;
    float mapOffX   = 0.0f;
    float mapOffZ   = 0.0f;
    float terrX1 = 0, terrZ1 = 0, terrX2 = 500, terrZ2 = 500;

    bool showBuildNotif = true;
    bool autoscrollChat = true;
    bool showDemoOnMap  = true;
    bool showTerr       = true;

    bool   hostCmdSent   = false;
    int    totalMessages = 0;
    int    totalDeals    = 0;
    double sessionStart  = 0;

    std::string cachedTopPlayer;
    int         cachedTopCount  = 0;
    size_t      cachedBuildsLen = 0;
    float mapMinX=9999,mapMaxX=-9999,mapMinZ=9999,mapMaxZ=-9999;
    size_t cachedMapB=0, cachedMapD=0;
} g;

static ID3D11Device*           g_dev   = NULL;
static ID3D11DeviceContext*    g_ctx   = NULL;
static IDXGISwapChain*         g_chain = NULL;
static ID3D11RenderTargetView* g_rtv   = NULL;
static HWND                    g_hwnd  = NULL;
static bool                    g_running = true;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static bool NetSend(SOCKET s, const char* b, int n)
{
    int sent=0;
    while(sent<n){int r=send(s,b+sent,n-sent,0);if(r<=0)return false;sent+=r;}
    return true;
}

static bool NetRecv(SOCKET s, char* b, int n)
{
    int got=0;
    while(got<n){int r=recv(s,b+got,n-got,0);if(r<=0)return false;got+=r;}
    return true;
}

static void NetDrain(SOCKET s, DWORD n)
{
    char t[512];
    while(n>0){DWORD k=n>512?512:n;if(!NetRecv(s,t,k))break;n-=k;}
}

static void NetSendMsg(SOCKET s, BYTE type, const void* data, DWORD sz)
{
    if(s==INVALID_SOCKET)return;
    MsgHeader h={type,sz};
    EnterCriticalSection(&g.sendLock);
    NetSend(s,(char*)&h,sizeof(h));
    if(data&&sz)NetSend(s,(char*)data,sz);
    LeaveCriticalSection(&g.sendLock);
}

static const char* g_saveDir =
    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
    "SovietRepublic\\media_soviet\\save\\mp_client";

static char* FileRead(const char* p, DWORD* sz)
{
    HANDLE h=CreateFileA(p,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE){*sz=0;return nullptr;}
    *sz=GetFileSize(h,NULL);
    char* b=(char*)malloc(*sz);
    if(!b){CloseHandle(h);*sz=0;return nullptr;}
    DWORD rd; ReadFile(h,b,*sz,&rd,NULL); CloseHandle(h); return b;
}

static void FileWrite(const char* p, const char* d, DWORD sz)
{
    HANDLE h=CreateFileA(p,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(h==INVALID_HANDLE_VALUE)return;
    DWORD wr; WriteFile(h,d,sz,&wr,NULL); CloseHandle(h);
}

static int PatchRead(const struct bspatch_stream* s, void* b, int n)
{
    PatchStream* ps=(PatchStream*)s->opaque;
    if(ps->pos+n>ps->size)return -1;
    memcpy(b,ps->buf+ps->pos,n); ps->pos+=n; return 0;
}

static void RecvFull(SOCKET s, const char* path)
{
    DWORD oSz=0,cSz=0;
    if(!NetRecv(s,(char*)&oSz,4)||!NetRecv(s,(char*)&cSz,4))return;
    if(!oSz||!cSz)return;
    char* cb=(char*)malloc(cSz); if(!cb)return;
    NetRecv(s,cb,cSz);
    char* ob=(char*)malloc(oSz); if(!ob){free(cb);return;}
    size_t r=ZSTD_decompress(ob,oSz,cb,cSz); free(cb);
    if(!ZSTD_isError(r))FileWrite(path,ob,(DWORD)r); free(ob);
}

static void RecvDiff(SOCKET s, const char* path)
{
    DWORD nSz=0,rSz=0,cSz=0;
    if(!NetRecv(s,(char*)&nSz,4)||!NetRecv(s,(char*)&rSz,4)||!NetRecv(s,(char*)&cSz,4))return;
    if(!nSz||!rSz||!cSz)return;
    char* cd=(char*)malloc(cSz); if(!cd)return; NetRecv(s,cd,cSz);
    char* rd=(char*)malloc(rSz); if(!rd){free(cd);return;}
    size_t dr=ZSTD_decompress(rd,rSz,cd,cSz); free(cd);
    if(ZSTD_isError(dr)){free(rd);return;}
    DWORD oSz=0; char* od=FileRead(path,&oSz);
    if(!od){free(rd);return;}
    char* nd=(char*)malloc(nSz); if(!nd){free(rd);free(od);return;}
    PatchStream ps={rd,0,rSz};
    struct bspatch_stream bs={}; bs.opaque=&ps; bs.read=PatchRead;
    if(bspatch((uint8_t*)od,oSz,(uint8_t*)nd,nSz,&bs)==0)FileWrite(path,nd,nSz);
    free(rd); free(od); free(nd);
}

static void SaveServer(const char* ip, const char* port, const char* name)
{
    for(int i=0;i<g.savedServerCount;i++)
        if(!strcmp(g.savedServers[i].ip,ip)&&!strcmp(g.savedServers[i].port,port))return;
    if(g.savedServerCount>=MAX_SAVED_SERVERS)return;
    auto& s=g.savedServers[g.savedServerCount++];
    strncpy(s.ip,ip,63); strncpy(s.port,port,7); strncpy(s.name,name,63);
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(s.lastSeen,32,"%02d.%02d %02d:%02d",st.wDay,st.wMonth,st.wHour,st.wMinute);
}

static void DeleteServer(int idx)
{
    if(idx<0||idx>=g.savedServerCount)return;
    for(int i=idx;i<g.savedServerCount-1;i++)g.savedServers[i]=g.savedServers[i+1];
    g.savedServerCount--;
}

static bool ResInCat(const std::string& nm, int cat)
{
    if(cat==g_catCount-1){
        for(int c=0;c<g_catCount-1;c++)if(ResInCat(nm,c))return false;
        return true;
    }
    for(int i=0;g_cats[cat].kw[i];i++)if(nm.find(g_cats[cat].kw[i])==0)return true;
    return false;
}

static void LoadResources()
{
    g.allResources.clear();
    char pat[MAX_PATH];
    snprintf(pat,MAX_PATH,
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
        "SovietRepublic\\media_soviet\\resources\\*.png");
    WIN32_FIND_DATAA fd;
    HANDLE h=FindFirstFileA(pat,&fd);
    if(h==INVALID_HANDLE_VALUE)return;
    static const char* skip[]={"ships","airplanes","helicopters","trains",
        "vehicles","workers","haldauhlealpha","service_material",NULL};
    do {
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        std::string nm=fd.cFileName;
        auto d=nm.rfind('.');if(d!=std::string::npos)nm=nm.substr(0,d);
        if(nm.find("_vehicle")!=std::string::npos)continue;
        if(nm.find("_mask")!=std::string::npos)continue;
        bool bad=false;
        for(int i=0;skip[i];i++)if(nm==skip[i]){bad=true;break;}
        if(bad)continue;
        g.allResources.push_back(nm);
    } while(FindNextFileA(h,&fd));
    FindClose(h);
    std::sort(g.allResources.begin(),g.allResources.end());
}

static void FilterCat(int cat)
{
    g.catResources.clear(); g.selectedRes=-1;
    std::string flt=g.resFilter;
    std::transform(flt.begin(),flt.end(),flt.begin(),::tolower);
    for(auto& r:g.allResources){
        if(!ResInCat(r,cat))continue;
        if(!flt.empty()){
            std::string lo=r;
            std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
            if(lo.find(flt)==std::string::npos)continue;
        }
        g.catResources.push_back(r);
    }
}

static void AddChat(const std::string& text, ChatType type=CHAT_NORMAL)
{
    ChatMsg m; m.text=text; m.type=type; m.ts=ImGui::GetTime();
    g.chat.push_back(m); g.totalMessages++;
    if(g.chat.size()>MAX_CHAT)g.chat.erase(g.chat.begin(),g.chat.begin()+30);
}

static void AddNotif(const std::string& text, ImVec4 col=ImVec4(0.9f,0.8f,0.2f,1.f))
{
    Notification n; n.text=text; n.color=col; n.ts=ImGui::GetTime();
    g.notifications.push_back(n);
    if(g.notifications.size()>4)g.notifications.erase(g.notifications.begin());
}

static void AddConnLog(const std::string& text, ImVec4 col=ImVec4(0.75f,0.75f,0.75f,1.f))
{
    ConnLogEntry e; e.text=text; e.color=col; e.ts=ImGui::GetTime();
    g.connLog.push_back(e);
    if(g.connLog.size()>MAX_CONN_LOG)g.connLog.erase(g.connLog.begin(),g.connLog.begin()+10);
}

static void UpdateBuildStats()
{
    if(g.builds.size()==g.cachedBuildsLen)return;
    g.cachedBuildsLen=g.builds.size();
    std::map<std::string,int> cnts;
    for(auto& b:g.builds)cnts[b.player]++;
    g.cachedTopPlayer=""; g.cachedTopCount=0;
    for(auto& kv:cnts)if(kv.second>g.cachedTopCount){g.cachedTopCount=kv.second;g.cachedTopPlayer=kv.first;}
}

static void UpdateMapBounds()
{
    if(g.builds.size()==g.cachedMapB&&g.demols.size()==g.cachedMapD)return;
    g.cachedMapB=g.builds.size(); g.cachedMapD=g.demols.size();
    g.mapMinX=9999;g.mapMaxX=-9999;g.mapMinZ=9999;g.mapMaxZ=-9999;
    for(auto& b:g.builds){g.mapMinX=std::min(g.mapMinX,b.x);g.mapMaxX=std::max(g.mapMaxX,b.x);g.mapMinZ=std::min(g.mapMinZ,b.z);g.mapMaxZ=std::max(g.mapMaxZ,b.z);}
    for(auto& d:g.demols){g.mapMinX=std::min(g.mapMinX,d.x);g.mapMaxX=std::max(g.mapMaxX,d.x);g.mapMinZ=std::min(g.mapMinZ,d.z);g.mapMaxZ=std::max(g.mapMaxZ,d.z);}
}

static void GetLocalZeroTierIP()
{
    ULONG bufLen=15000;
    char* buf=(char*)malloc(bufLen); if(!buf)return;
    if(GetAdaptersAddresses(AF_INET,0,NULL,(IP_ADAPTER_ADDRESSES*)buf,&bufLen)==NO_ERROR){
        for(auto* aa=(IP_ADAPTER_ADDRESSES*)buf;aa;aa=aa->Next){
            char friendly[256]={},desc[256]={};
            WideCharToMultiByte(CP_ACP,0,aa->FriendlyName,-1,friendly,255,NULL,NULL);
            WideCharToMultiByte(CP_ACP,0,aa->Description,-1,desc,255,NULL,NULL);
            if(strstr(aa->AdapterName,"ZeroTier")||strstr(friendly,"ZeroTier")||strstr(desc,"ZeroTier")){
                for(auto* ua=aa->FirstUnicastAddress;ua;ua=ua->Next){
                    auto* sin=(sockaddr_in*)ua->Address.lpSockaddr;
                    if(sin->sin_family==AF_INET)inet_ntop(AF_INET,&sin->sin_addr,g_myZeroTierIP,64);
                }
            }
        }
    }
    free(buf);
}

static DWORD WINAPI RecvThread(LPVOID)
{
    while(g.connected){
        MsgHeader hdr;
        if(!NetRecv(g.sock,(char*)&hdr,sizeof(hdr))){
            g.connected=false; g_networkOnline=false;
            AddChat(g_lang==LANG_RU?"[SYSTEM] Соединение потеряно":"[SYSTEM] Connection lost",CHAT_SYSTEM);
            AddConnLog(g_lang==LANG_RU?"Соединение потеряно":"Connection lost",ImVec4(0.9f,0.3f,0.3f,1.f));
            break;
        }
        switch(hdr.type){
        case MSG_PING:
            NetDrain(g.sock,hdr.size);
            NetSendMsg(g.sock,MSG_PONG,nullptr,0);
            break;
        case MSG_PONG:
            NetDrain(g.sock,hdr.size);
            g.ping=(DWORD)(GetTickCount64()-g.lastPingTime);
            g.pingHistory.push_back((float)g.ping);
            if(g.pingHistory.size()>PING_HISTORY)g.pingHistory.erase(g.pingHistory.begin());
            for(auto& p:g.players)if(p.name==g.name)p.ping=g.ping;
            break;
        case MSG_BUILD:
            if(hdr.size==sizeof(BuildCmd)){
                BuildCmd c; NetRecv(g.sock,(char*)&c,sizeof(c));
                BuildLog bl; bl.player=c.playerName; bl.type=c.typeName;
                bl.x=c.x; bl.z=c.z; bl.ts=ImGui::GetTime();
                g.builds.push_back(bl);
                char buf[256];
                snprintf(buf,256,"[BUILD] %s placed %s at (%.0f,%.0f)",c.playerName,c.typeName,c.x,c.z);
                AddChat(buf,CHAT_BUILD);
                if(g.showBuildNotif){
                    char nb[192]; snprintf(nb,192,"%s built %s",c.playerName,c.typeName);
                    AddNotif(nb,ImVec4(0.4f,0.85f,0.4f,1.f));
                }
                for(auto& p:g.players)if(p.name==std::string(c.playerName))p.builds++;
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_DEMOLISH:
            if(hdr.size==sizeof(DemolishCmd)){
                DemolishCmd c; NetRecv(g.sock,(char*)&c,sizeof(c));
                DemolLog dl; dl.player=c.playerName; dl.x=c.x; dl.z=c.z; dl.ts=ImGui::GetTime();
                g.demols.push_back(dl);
                char buf[192];
                snprintf(buf,192,"[DEMO] %s demolished at (%.0f,%.0f)",c.playerName,c.x,c.z);
                AddChat(buf,CHAT_DEMO);
                for(auto& p:g.players)if(p.name==std::string(c.playerName))p.demolishes++;
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_RESOURCE_REQ:
            if(hdr.size==sizeof(ResourceReq)){
                ResourceReq r; NetRecv(g.sock,(char*)&r,sizeof(r));
                ResourceDeal d; d.from=r.fromPlayer; d.to=r.toPlayer;
                d.resource=r.resource; d.amount=r.amount; d.price=r.price;
                d.pending=true; d.accepted=false; d.ts=ImGui::GetTime();
                g.deals.push_back(d);
                char buf[256];
                snprintf(buf,256,"[TRADE] %s requests %d x %s",r.fromPlayer,r.amount,r.resource);
                AddChat(buf,CHAT_TRADE);
                AddNotif(buf,ImVec4(0.9f,0.7f,0.1f,1.f));
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_CHAT:
            if(hdr.size>0&&hdr.size<4096){
                static char cb[4096];
                NetRecv(g.sock,cb,hdr.size); cb[hdr.size]=0;
                AddChat(cb,CHAT_NORMAL);
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_TERRITORY:
            if(hdr.size==sizeof(TerritoryPacket)){
                TerritoryPacket tp; NetRecv(g.sock,(char*)&tp,sizeof(tp));
                for(auto& p:g.players){
                    if(p.name==std::string(tp.playerName)){
                        p.hasTerr=(tp.action==1);
                        p.tx1=tp.x1;p.tz1=tp.z1;p.tx2=tp.x2;p.tz2=tp.z2;
                        p.terrColor=tp.color; break;
                    }
                }
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_SAVE_FULL:
        case MSG_SAVE_DIFF:{
            bool diff=(hdr.type==MSG_SAVE_DIFF);
            int fc=0; NetRecv(g.sock,(char*)&fc,4);
            g.syncing=true; g.progressMax=fc; g.progressCur=0;
            g.syncStatus=diff?"Receiving delta...":"Receiving save...";
            SHCreateDirectoryExA(NULL,g_saveDir,NULL);
            for(int i=0;i<fc;i++){
                int nl=0; NetRecv(g.sock,(char*)&nl,4);
                char fn[MAX_PATH]={}; int rl=nl<MAX_PATH-1?nl:MAX_PATH-1;
                NetRecv(g.sock,fn,rl); if(nl>rl)NetDrain(g.sock,nl-rl);
                char fp[MAX_PATH]; snprintf(fp,MAX_PATH,"%s\\%s",g_saveDir,fn);
                if(diff)RecvDiff(g.sock,fp); else RecvFull(g.sock,fp);
                g.progressCur=i+1;
            }
            g.syncing=false;
            g.syncStatus=diff?"Delta applied":"Done! Load mp_client save";
            AddChat(std::string("[SYNC] ")+g.syncStatus,CHAT_SYSTEM);
            break;}
        default: NetDrain(g.sock,hdr.size); break;
        }
    }
    return 0;
}

static void DoConnect()
{
    if(!g_pluginFound){
        AddChat(g_lang==LANG_RU?"[ERROR] Игра не запущена":"[ERROR] Game not running",CHAT_ERROR);
        AddNotif(g_lang==LANG_RU?"Игра не запущена!":"Game not running!",ImVec4(0.9f,0.3f,0.2f,1.f));
        return;
    }
    if(g.isHost){
        if(!g.hostCmdSent){
            g.hostCmdSent=true;
            g_userDisconnected=false;
            AddChat(g_lang==LANG_RU?"[SYSTEM] Режим хоста — сервер уже запущен плагином":"[SYSTEM] Host mode — server started by plugin",CHAT_SYSTEM);
            if(g_myZeroTierIP[0]){
                char msg[128]; snprintf(msg,128,"[SYSTEM] ZeroTier IP: %s",g_myZeroTierIP);
                AddChat(msg,CHAT_SYSTEM);
            }
            AddConnLog(g_lang==LANG_RU?"Хост-режим":"Host mode",ImVec4(0.3f,0.9f,0.3f,1.f));
        }
        return;
    }
    if(!g.ip[0]){
        AddChat(g_lang==LANG_RU?"[ERROR] Введи IP хоста":"[ERROR] Enter host IP",CHAT_ERROR);
        return;
    }
    char logMsg[128];
    snprintf(logMsg,128,"Connecting to %s:%s as %s",g.ip,g.port,g.name);
    AddChat(std::string("[SYSTEM] ")+logMsg,CHAT_SYSTEM);
    AddConnLog(logMsg,ImVec4(0.9f,0.7f,0.2f,1.f));
    g_shm.SendCommand(MP_CMD_CONNECT,g.ip,g.port,g.name);
    g.sessionStart=ImGui::GetTime();
    g_userDisconnected=false;
    SaveServer(g.ip,g.port,g.name);
    SaveSettings(g.name,g.ip,g.port);
    AddNotif(g_lang==LANG_RU?"Подключение...":"Connecting...",ImVec4(0.9f,0.7f,0.1f,1.f));
}

static void DoDisconnect()
{
    g_userDisconnected=true; g.connected=false; g_networkOnline=false;
    if(g.sock!=INVALID_SOCKET){closesocket(g.sock);g.sock=INVALID_SOCKET;}
    if(g.recvThread){WaitForSingleObject(g.recvThread,500);CloseHandle(g.recvThread);g.recvThread=NULL;}
    g.players.clear(); g.ping=0; g.pingHistory.clear(); g.hostCmdSent=false;
    if(g_pluginFound&&g_shm.block)g_shm.SendCommand(MP_CMD_DISCONNECT);
    AddChat(g_lang==LANG_RU?"[SYSTEM] Отключён":"[SYSTEM] Disconnected",CHAT_SYSTEM);
    AddConnLog(g_lang==LANG_RU?"Отключён":"Disconnected",ImVec4(0.8f,0.4f,0.4f,1.f));
}

static void DoSendChat()
{
    if(!g.chatInput[0])return;
    if(g_pluginFound&&g_shm.block){
        g_shm.SendCommand(MP_CMD_CHAT,g.chatInput);
        char full[320]; snprintf(full,320,"[%s]: %s",g.name,g.chatInput);
        AddChat(full,CHAT_NORMAL);
    } else {
        AddChat(g_lang==LANG_RU?"[ERROR] Плагин не подключён":"[ERROR] Plugin not connected",CHAT_ERROR);
    }
    g.chatInput[0]='\0'; ImGui::SetKeyboardFocusHere(-1);
}

static void DoSendRequest()
{
    if(g.selectedRes<0||g.selectedRes>=(int)g.catResources.size())return;
    ResourceReq rq={}; strncpy(rq.fromPlayer,g.name,63);
    strncpy(rq.resource,g.catResources[g.selectedRes].c_str(),63);
    rq.amount=atoi(g.amountBuf); rq.price=atoi(g.priceBuf);
    if(rq.amount<=0)return;
    if(g.connected&&g.sock!=INVALID_SOCKET)NetSendMsg(g.sock,MSG_RESOURCE_REQ,&rq,sizeof(rq));
    ResourceDeal d; d.from=g.name; d.resource=rq.resource;
    d.amount=rq.amount; d.price=rq.price; d.pending=false; d.accepted=false;
    d.ts=ImGui::GetTime(); g.deals.push_back(d); g.totalDeals++;
    char buf[256]; snprintf(buf,256,"[TRADE] Requested %d x %s",rq.amount,rq.resource);
    AddChat(buf,CHAT_TRADE);
    AddNotif(g_lang==LANG_RU?"Запрос отправлен!":"Request sent!",ImVec4(0.9f,0.7f,0.1f,1.f));
}

static void DoAdminAction(BYTE cmd)
{
    if(!g_networkOnline||!g.adminTarget[0])return;
    AdminPacket ap={}; ap.command=cmd;
    strncpy(ap.target,g.adminTarget,63);
    strncpy(ap.param,g.adminReason,127);
    if(g.connected&&g.sock!=INVALID_SOCKET)NetSendMsg(g.sock,MSG_ADMIN,&ap,sizeof(ap));
    char buf[128]; snprintf(buf,128,"[ADMIN] cmd=%d target=%s",cmd,g.adminTarget);
    AddChat(buf,CHAT_SYSTEM);
}

static void DoAdminBroadcast()
{
    if(!g_networkOnline||!g.adminMsg[0])return;
    char msg[320]; snprintf(msg,320,"[BROADCAST] %s",g.adminMsg);
    g_shm.SendCommand(MP_CMD_CHAT,msg);
    AddChat(msg,CHAT_SYSTEM);
    g.adminMsg[0]='\0';
}

static void DoSetTerritory(bool clear)
{
    TerritoryPacket tp={}; strncpy(tp.playerName,g.name,63);
    tp.action=clear?0:1;
    tp.x1=g.terrX1;tp.z1=g.terrZ1;tp.x2=g.terrX2;tp.z2=g.terrZ2;
    tp.color=IM_COL32(230,100,50,100);
    if(g.connected&&g.sock!=INVALID_SOCKET)NetSendMsg(g.sock,MSG_TERRITORY,&tp,sizeof(tp));
    AddChat(clear?"[TERR] Cleared":"[TERR] Set",CHAT_SYSTEM);
}

static void PollSharedMemory()
{
    static ULONGLONG lastPoll=0;
    ULONGLONG now=GetTickCount64();
    ULONGLONG interval=g_pluginFound?POLL_ONLINE_MS:POLL_OFFLINE_MS;
    if(now-lastPoll<interval)return;
    lastPoll=now;

    if(!g_pluginFound){
        if(g_shm.Create(false)){
            if(g_shm.block&&g_shm.block->magic==SHARED_MAGIC){
                g_pluginFound=true;
                AddChat(g_lang==LANG_RU?"[SYSTEM] Плагин подключён":"[SYSTEM] Plugin connected",CHAT_SYSTEM);
                AddNotif(g_lang==LANG_RU?"Плагин найден!":"Plugin found!",ImVec4(0.3f,0.9f,0.3f,1.f));
                AddConnLog(g_lang==LANG_RU?"Плагин найден":"Plugin found",ImVec4(0.3f,0.9f,0.3f,1.f));
                GetLocalZeroTierIP();
                if(g_myZeroTierIP[0]){
                    char msg[128]; snprintf(msg,128,"ZeroTier: %s",g_myZeroTierIP);
                    AddConnLog(msg,ImVec4(0.9f,0.7f,0.2f,1.f));
                }
            }
        }
        return;
    }
    if(!g_shm.block)return;
    if(!g_shm.Lock(30))return;

    g_pluginStatus=g_shm.block->status;
    strncpy(g_pluginStatusText,g_shm.block->statusText,255);
    g_shmUptime=g_shm.block->sessionUptime;
    g_shmBuilds=g_shm.block->totalBuilds;

    DWORD pc=g_shm.block->playerCount;
    g.players.clear();
    for(DWORD i=0;i<pc&&i<4;i++){
        auto& ps=g_shm.block->players[i];
        if(!ps.connected)continue;
        PlayerInfo pi; pi.name=ps.name; pi.ping=ps.ping;
        pi.connected=true; pi.builds=0; pi.demolishes=0;
        pi.hasTerr=false; pi.terrColor=0;
        pi.color=g_playerColors[i%4];
        g.players.push_back(pi);
    }

    static DWORD s_lastBuild=0;
    DWORD nc=g_shm.block->buildNotifyCount;
    if(nc>s_lastBuild){
        for(DWORD i=s_lastBuild;i<nc;i++){
            auto& bn=g_shm.block->buildNotify[i%MAX_BUILD_NOTIFY];
            BuildLog bl; bl.player=bn.playerName; bl.type=bn.typeName;
            bl.x=bn.x; bl.z=bn.z; bl.ts=ImGui::GetTime();
            g.builds.push_back(bl);
            char msg[256];
            snprintf(msg,256,"[BUILD] %s placed %s at (%.0f,%.0f)",
                     bn.playerName,bn.typeName,bn.x,bn.z);
            AddChat(msg,CHAT_BUILD);
        }
        s_lastBuild=nc;
    }

    bool wasOnline=g_networkOnline;
    if(!g_userDisconnected){
        bool isHost=(g_pluginStatus==MP_STATUS_HOST);
        bool isConn=(g_pluginStatus==MP_STATUS_CONNECTED);
        bool serverMode=g.isHost&&(g_pluginStatus!=MP_STATUS_OFFLINE);
        g_networkOnline=(isHost||isConn||serverMode);
        g.connected=g_networkOnline;
        if(g_networkOnline&&g.sessionStart<=0)g.sessionStart=ImGui::GetTime();
    } else {g_networkOnline=false;g.connected=false;}
    if(!wasOnline&&g_networkOnline)
        AddConnLog(g_lang==LANG_RU?"Подключён!":"Connected!",ImVec4(0.3f,0.9f,0.3f,1.f));

    g_shm.Unlock();
}

static void ApplyTheme()
{
    ImGuiStyle& s=ImGui::GetStyle();
    s.WindowRounding=0.f;s.ChildRounding=6.f;s.FrameRounding=4.f;
    s.GrabRounding=4.f;s.TabRounding=4.f;s.ScrollbarRounding=4.f;
    s.FramePadding=ImVec2(8,5);s.ItemSpacing=ImVec2(8,6);
    s.WindowPadding=ImVec2(10,10);s.ScrollbarSize=11.f;
    s.GrabMinSize=8.f;s.WindowBorderSize=0.f;s.ChildBorderSize=1.f;
    ImVec4* c=s.Colors;
    c[ImGuiCol_WindowBg]            =ImVec4(0.11f,0.11f,0.14f,1.f);
    c[ImGuiCol_ChildBg]             =ImVec4(0.09f,0.09f,0.12f,1.f);
    c[ImGuiCol_PopupBg]             =ImVec4(0.12f,0.12f,0.16f,0.97f);
    c[ImGuiCol_Border]              =ImVec4(0.25f,0.25f,0.32f,0.60f);
    c[ImGuiCol_FrameBg]             =ImVec4(0.16f,0.16f,0.21f,1.f);
    c[ImGuiCol_FrameBgHovered]      =ImVec4(0.20f,0.20f,0.27f,1.f);
    c[ImGuiCol_FrameBgActive]       =ImVec4(0.24f,0.24f,0.32f,1.f);
    c[ImGuiCol_TitleBgActive]       =ImVec4(0.55f,0.12f,0.08f,1.f);
    c[ImGuiCol_ScrollbarBg]         =ImVec4(0.08f,0.08f,0.11f,1.f);
    c[ImGuiCol_ScrollbarGrab]       =ImVec4(0.30f,0.10f,0.07f,1.f);
    c[ImGuiCol_ScrollbarGrabHovered]=ImVec4(0.45f,0.14f,0.10f,1.f);
    c[ImGuiCol_ScrollbarGrabActive] =ImVec4(0.60f,0.18f,0.13f,1.f);
    c[ImGuiCol_CheckMark]           =ImVec4(0.90f,0.35f,0.22f,1.f);
    c[ImGuiCol_SliderGrab]          =ImVec4(0.75f,0.22f,0.14f,1.f);
    c[ImGuiCol_SliderGrabActive]    =ImVec4(0.90f,0.28f,0.18f,1.f);
    c[ImGuiCol_Button]              =ImVec4(0.58f,0.14f,0.09f,1.f);
    c[ImGuiCol_ButtonHovered]       =ImVec4(0.72f,0.19f,0.12f,1.f);
    c[ImGuiCol_ButtonActive]        =ImVec4(0.85f,0.25f,0.16f,1.f);
    c[ImGuiCol_Header]              =ImVec4(0.48f,0.12f,0.08f,1.f);
    c[ImGuiCol_HeaderHovered]       =ImVec4(0.62f,0.16f,0.11f,1.f);
    c[ImGuiCol_HeaderActive]        =ImVec4(0.75f,0.20f,0.14f,1.f);
    c[ImGuiCol_Separator]           =ImVec4(0.25f,0.25f,0.32f,1.f);
    c[ImGuiCol_Tab]                 =ImVec4(0.14f,0.14f,0.19f,1.f);
    c[ImGuiCol_TabHovered]          =ImVec4(0.68f,0.18f,0.12f,1.f);
    c[ImGuiCol_TabActive]           =ImVec4(0.62f,0.15f,0.10f,1.f);
    c[ImGuiCol_TabUnfocused]        =ImVec4(0.10f,0.10f,0.14f,1.f);
    c[ImGuiCol_TabUnfocusedActive]  =ImVec4(0.38f,0.10f,0.07f,1.f);
    c[ImGuiCol_Text]                =ImVec4(0.90f,0.88f,0.85f,1.f);
    c[ImGuiCol_TextDisabled]        =ImVec4(0.48f,0.46f,0.44f,1.f);
    c[ImGuiCol_PlotLines]           =ImVec4(0.75f,0.25f,0.15f,1.f);
    c[ImGuiCol_PlotHistogram]       =ImVec4(0.75f,0.25f,0.15f,1.f);
}

static ImVec4 ChatColor(ChatType t)
{
    switch(t){
    case CHAT_SYSTEM: return ImVec4(0.5f,0.8f,0.5f,1.f);
    case CHAT_BUILD:  return ImVec4(0.4f,0.75f,0.95f,1.f);
    case CHAT_TRADE:  return ImVec4(0.95f,0.75f,0.25f,1.f);
    case CHAT_ERROR:  return ImVec4(0.95f,0.3f,0.3f,1.f);
    case CHAT_DEMO:   return ImVec4(0.95f,0.55f,0.2f,1.f);
    default:          return ImVec4(0.88f,0.86f,0.84f,1.f);
    }
}

static void SectionHeader(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.85f,0.28f,0.18f,1.f));
    ImGui::Text("%s",label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

static std::string FormatTime(double t)
{
    double e=ImGui::GetTime()-t;
    char buf[32];
    if(e<60)snprintf(buf,32,"%.0fs ago",e);
    else if(e<3600)snprintf(buf,32,"%.0fm ago",e/60);
    else snprintf(buf,32,"%.0fh ago",e/3600);
    return buf;
}

static std::string SessionDuration()
{
    if(!g_networkOnline||g.sessionStart<=0)return "";
    double e=ImGui::GetTime()-g.sessionStart;
    int h=(int)(e/3600),m=(int)(fmod(e,3600)/60),s=(int)fmod(e,60);
    char buf[32];
    if(h>0)snprintf(buf,32,"%dh %dm",h,m);
    else snprintf(buf,32,"%dm %ds",m,s);
    return buf;
}

static void DrawStatusDot(bool ok, float r=5.f)
{
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 p=ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x+r,p.y+r+2),r,
        ok?IM_COL32(60,210,80,255):IM_COL32(200,60,60,255));
    ImGui::Dummy(ImVec2(r*2+4,r*2));
}

static void DrawPingBar(DWORD ping)
{
    ImVec4 col=ping<80?ImVec4(0.3f,0.85f,0.3f,1.f):
               ping<150?ImVec4(0.9f,0.75f,0.1f,1.f):ImVec4(0.9f,0.25f,0.2f,1.f);
    ImGui::TextColored(col,"%dms",(int)ping);
}

static void TabMain()
{
    float W=ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.10f,0.10f,0.14f,1.f));
    ImGui::BeginChild("##status",ImVec2(W,54),true);
    ImGui::Text("%s",L().pluginLabel);ImGui::SameLine(70);DrawStatusDot(g_pluginFound);ImGui::SameLine();
    if(g_pluginFound)ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1.f),"%s",L().gameRunning);
    else ImGui::TextDisabled("%s",L().waitingGame);
    ImGui::Text("%s",L().networkLabel);ImGui::SameLine(70);DrawStatusDot(g_networkOnline);ImGui::SameLine();
    if(g_networkOnline){
        ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1.f),"%s",g_pluginStatusText);
        ImGui::SameLine(0,16);
        std::string dur=SessionDuration();
        if(!dur.empty())ImGui::TextDisabled("  %s",dur.c_str());
        if(g.ping>0){ImGui::SameLine(0,16);DrawPingBar(g.ping);}
        ImGui::SameLine(0,16);ImGui::TextDisabled("[%s]",g.name);
    } else ImGui::TextDisabled("%s",g_pluginStatusText);
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.10f,0.10f,0.14f,1.f));
    float connH=g_networkOnline?36.f:140.f;
    ImGui::BeginChild("##conn",ImVec2(W,connH),true);
    if(!g_networkOnline){
        ImGui::Text("%s",L().modeLabel);ImGui::SameLine(60);
        if(ImGui::RadioButton(L().modeHost,g.isHost))g.isHost=true;
        ImGui::SameLine();
        if(ImGui::RadioButton(L().modeClient,!g.isHost))g.isHost=false;
        ImGui::SameLine(0,20);
        ImGui::Text("%s",L().gameLabel);ImGui::SameLine();
        if(ImGui::RadioButton(L().gameCoop,g.isCoopMode))g.isCoopMode=true;
        ImGui::SameLine();
        if(ImGui::RadioButton(L().gameTerr,!g.isCoopMode))g.isCoopMode=false;
        if(!g.isHost){
            ImGui::SetNextItemWidth(160);ImGui::InputText("IP",g.ip,64);
            ImGui::SameLine();ImGui::SetNextItemWidth(50);ImGui::InputText(L().portLabel,g.port,8);
            ImGui::SameLine();ImGui::SetNextItemWidth(90);ImGui::InputText(L().nameLabel,g.name,64);
        } else {
            ImGui::SetNextItemWidth(200);ImGui::InputText(L().nameLabel,g.name,64);
            if(g_myZeroTierIP[0]){
                ImGui::SameLine(0,16);
                ImGui::TextDisabled("ZeroTier:");ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.f),"%s",g_myZeroTierIP);
            }
        }
        ImGui::Spacing();
        bool pluginReady=g_pluginFound&&g_pluginStatus!=MP_STATUS_OFFLINE;
        if(g.isHost&&!pluginReady)ImGui::BeginDisabled();
        if(ImGui::Button(g.isHost?L().btnStartHost:L().btnConnect,ImVec2(W-12,26)))DoConnect();
        if(g.isHost&&!pluginReady){ImGui::EndDisabled();ImGui::TextDisabled(g_lang==LANG_RU?"  Сначала запусти игру":"  Start the game first");}
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.28f,0.28f,0.36f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.35f,0.35f,0.45f,1.f));
        if(ImGui::Button(L().btnDisconnect,ImVec2(W-12,24)))DoDisconnect();
        ImGui::PopStyleColor(2);
        if(g.isHost){
            ImGui::TextDisabled("  Host | port: %s | %d/%d players",g.port,(int)g.players.size(),4);
        } else {
            ImGui::TextDisabled("  Connected to %s:%s",g.ip,g.port);
        }
    }
    if(g.syncing){
        ImGui::Spacing();
        ImGui::ProgressBar((float)g.progressCur/std::max(1,g.progressMax),ImVec2(W-12,8),"");
        ImGui::TextDisabled("  %s (%d/%d)",g.syncStatus.c_str(),g.progressCur,g.progressMax);
    }
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::Spacing();

    float half=(W-6)*0.5f;
    ImGui::BeginChild("##players",ImVec2(half,120),true);
    SectionHeader(L().playersOnline);
    if(g.players.empty())ImGui::TextDisabled("%s",L().noPlayers);
    else for(auto& p:g.players){
        ImDrawList* pdl=ImGui::GetWindowDrawList();
        ImVec2 pp=ImGui::GetCursorScreenPos();
        ImU32 pc=IM_COL32((int)(p.color.x*255),(int)(p.color.y*255),(int)(p.color.z*255),255);
        pdl->AddCircleFilled(ImVec2(pp.x+6,pp.y+8),5.f,pc);
        ImGui::Dummy(ImVec2(14,0));ImGui::SameLine();
        ImGui::Text("%s",p.name.c_str());
        if(p.ping>0){ImGui::SameLine();DrawPingBar(p.ping);}
        ImGui::SameLine(0,8);ImGui::TextDisabled("b:%d",p.builds);
    }
    ImGui::EndChild();ImGui::SameLine(0,6);

    ImGui::BeginChild("##recentbuilds",ImVec2(half,120),true);
    SectionHeader(L().recentBuilds);
    int bn=(int)g.builds.size(),bstart=bn>5?bn-5:0;
    if(bn==0)ImGui::TextDisabled("%s",L().noBuildsYet);
    for(int i=bstart;i<bn;i++){
        auto& b=g.builds[i];
        ImGui::TextColored(ImVec4(0.75f,0.25f,0.15f,1.f),"%.8s",b.player.c_str());
        ImGui::SameLine();ImGui::Text("%.20s",b.type.c_str());
    }
    ImGui::EndChild();ImGui::Spacing();

    float chatH=ImGui::GetContentRegionAvail().y-34.f;if(chatH<60)chatH=60;
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.08f,0.08f,0.11f,1.f));
    ImGui::BeginChild("##chat",ImVec2(W,chatH),true);
    bool scrollToBot=(ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-4);
    {
        ImGuiListClipper clip;
        clip.Begin((int)g.chat.size());
        while(clip.Step())
            for(int ci=clip.DisplayStart;ci<clip.DisplayEnd;ci++)
                ImGui::TextColored(ChatColor(g.chat[ci].type),"%s",g.chat[ci].text.c_str());
        clip.End();
    }
    if(scrollToBot&&g.autoscrollChat)ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(W-56);
    bool enter=ImGui::InputText("##ci",g.chatInput,256,ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if((ImGui::Button(L().sendBtn,ImVec2(48,0))||enter)&&g_pluginFound)DoSendChat();
}

static void TabImport()
{
    float W=ImGui::GetContentRegionAvail().x;
    float half=(W-6)*0.5f;

    ImGui::BeginChild("##catpick",ImVec2(half,220),true);
    SectionHeader(L().categoryHdr);
    for(int i=0;i<g_catCount;i++){
        bool sel=(g.selectedCat==i);
        if(ImGui::Selectable(g_cats[i].name,sel,0,ImVec2(0,20))){g.selectedCat=i;FilterCat(i);}
    }
    ImGui::EndChild();ImGui::SameLine(0,6);

    ImGui::BeginChild("##respick",ImVec2(half,220),true);
    SectionHeader(L().resourceHdr);
    ImGui::SetNextItemWidth(half-20);
    if(ImGui::InputText("##rf",g.resFilter,64))FilterCat(g.selectedCat);
    ImGui::Separator();
    for(int i=0;i<(int)g.catResources.size();i++){
        bool sel=(g.selectedRes==i);
        if(ImGui::Selectable(g.catResources[i].c_str(),sel,0,ImVec2(0,20)))g.selectedRes=i;
    }
    if(g.catResources.empty())ImGui::TextDisabled("  --");
    ImGui::EndChild();ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.10f,0.10f,0.14f,1.f));
    ImGui::BeginChild("##reqform",ImVec2(W,90),true);
    SectionHeader(L().sendReqHdr);
    const char* rname=(g.selectedRes>=0&&g.selectedRes<(int)g.catResources.size())
                      ?g.catResources[g.selectedRes].c_str():L().selectResource;
    ImGui::Text("%s",L().resourceLabel);ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f,0.65f,0.20f,1.f),"%s",rname);
    ImGui::SetNextItemWidth(110);ImGui::InputText(L().amountLabel,g.amountBuf,16,ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(0,16);ImGui::SetNextItemWidth(110);ImGui::InputText(L().priceLabel,g.priceBuf,16,ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(0,8);ImGui::TextDisabled("%s",L().priceFree);ImGui::SameLine(0,16);
    bool canSend=g.selectedRes>=0;if(!canSend)ImGui::BeginDisabled();
    if(ImGui::Button(L().sendReqBtn,ImVec2(120,22)))DoSendRequest();
    if(!canSend)ImGui::EndDisabled();
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::Spacing();

    ImGui::BeginChild("##deals",ImVec2(W,0),true);
    SectionHeader(L().dealsHdr);
    if(g.deals.empty()){ImGui::TextDisabled("%s",L().noDeals);}
    else{
        ImGui::Columns(5,"dc",true);
        ImGui::SetColumnWidth(0,80);ImGui::SetColumnWidth(1,80);
        ImGui::SetColumnWidth(2,120);ImGui::SetColumnWidth(3,60);ImGui::SetColumnWidth(4,70);
        ImGui::TextDisabled("%s",L().fromCol);ImGui::NextColumn();
        ImGui::TextDisabled("%s",L().toCol);ImGui::NextColumn();
        ImGui::TextDisabled("Resource");ImGui::NextColumn();
        ImGui::TextDisabled("Amt");ImGui::NextColumn();
        ImGui::TextDisabled("%s",L().statusCol);ImGui::NextColumn();
        ImGui::Separator();
        for(int i=(int)g.deals.size()-1;i>=0;i--){
            auto& d=g.deals[i];
            ImGui::Text("%s",d.from.c_str());ImGui::NextColumn();
            ImGui::Text("%s",d.to.empty()?L().allStr:d.to.c_str());ImGui::NextColumn();
            ImGui::Text("%s",d.resource.c_str());ImGui::NextColumn();
            ImGui::Text("%d",d.amount);ImGui::NextColumn();
            if(d.pending){
                ImGui::TextColored(ImVec4(0.9f,0.7f,0.1f,1.f),"%s",L().pendingStr);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.15f,0.55f,0.20f,1.f));
                char ab[16];snprintf(ab,16,"A##%d",i);
                if(ImGui::SmallButton(ab)){
                    d.pending=false;d.accepted=true;
                    AddNotif(g_lang==LANG_RU?"Сделка принята!":"Deal accepted!",ImVec4(0.3f,0.9f,0.3f,1.f));
                }
                ImGui::PopStyleColor();ImGui::SameLine();
                char db[16];snprintf(db,16,"D##%d",i);
                if(ImGui::SmallButton(db)){d.pending=false;d.accepted=false;}
            } else {
                if(d.accepted)ImGui::TextColored(ImVec4(0.3f,0.85f,0.35f,1.f),"%s",L().acceptedStr);
                else ImGui::TextDisabled("%s",L().declinedStr);
            }
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();
}

static void TabBuildings()
{
    float W=ImGui::GetContentRegionAvail().x;
    if(ImGui::BeginTabBar("##btabs")){

        if(ImGui::BeginTabItem(L().buildHistHdr)){
            UpdateBuildStats();
            ImGui::BeginChild("##bstats",ImVec2(W,44),true);
            ImGui::Columns(3,"bc",false);
            ImGui::TextDisabled("%s",L().totalPlaced);ImGui::SameLine();ImGui::Text("%d",(int)g.builds.size());
            ImGui::NextColumn();
            if(!g.cachedTopPlayer.empty()){
                ImGui::TextDisabled("%s",L().mostActive);ImGui::SameLine();
                ImGui::Text("%s (%d)",g.cachedTopPlayer.c_str(),g.cachedTopCount);
            }
            ImGui::NextColumn();
            ImGui::TextDisabled("%s",L().filterLabel);ImGui::SameLine();
            ImGui::SetNextItemWidth(120);ImGui::InputText("##bf",g.buildFilter,64);
            ImGui::Columns(1);
            ImGui::EndChild();ImGui::Spacing();
            ImGui::BeginChild("##buildlog",ImVec2(W,0),true);
            if(g.builds.empty()){ImGui::TextDisabled("%s",L().noBuildingsYet);}
            else{
                std::string flt=g.buildFilter;
                std::transform(flt.begin(),flt.end(),flt.begin(),::tolower);
                ImGui::Columns(5,"bl",true);
                ImGui::SetColumnWidth(0,90);ImGui::SetColumnWidth(1,160);
                ImGui::SetColumnWidth(2,60);ImGui::SetColumnWidth(3,60);ImGui::SetColumnWidth(4,80);
                ImGui::TextDisabled("%s",L().playerCol);ImGui::NextColumn();
                ImGui::TextDisabled("%s",L().buildingCol);ImGui::NextColumn();
                ImGui::TextDisabled("X");ImGui::NextColumn();
                ImGui::TextDisabled("Z");ImGui::NextColumn();
                ImGui::TextDisabled("%s",L().whenCol);ImGui::NextColumn();
                ImGui::Separator();
                ImGuiListClipper clip;
                clip.Begin((int)g.builds.size());
                while(clip.Step()){
                    for(int i=clip.DisplayEnd-1;i>=clip.DisplayStart;i--){
                        auto& b=g.builds[i];
                        if(!flt.empty()){
                            std::string lo=b.type+b.player;
                            std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
                            if(lo.find(flt)==std::string::npos)continue;
                        }
                        ImVec4 pc=ImVec4(0.9f,0.4f,0.2f,1.f);
                        for(auto& p:g.players)if(p.name==b.player){pc=p.color;break;}
                        ImGui::TextColored(pc,"%.8s",b.player.c_str());ImGui::NextColumn();
                        ImGui::Text("%.22s",b.type.c_str());ImGui::NextColumn();
                        ImGui::Text("%.0f",b.x);ImGui::NextColumn();
                        ImGui::Text("%.0f",b.z);ImGui::NextColumn();
                        ImGui::TextDisabled("%s",FormatTime(b.ts).c_str());ImGui::NextColumn();
                    }
                }
                clip.End();
                ImGui::Columns(1);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem(L().demolHistHdr)){
            ImGui::BeginChild("##dstats",ImVec2(W,44),true);
            ImGui::Columns(2,"dc2",false);
            ImGui::TextDisabled("%s",L().totalDemol);ImGui::SameLine();ImGui::Text("%d",(int)g.demols.size());
            ImGui::NextColumn();
            ImGui::TextDisabled("%s",L().filterLabel);ImGui::SameLine();
            ImGui::SetNextItemWidth(120);ImGui::InputText("##df",g.demolFilter,64);
            ImGui::Columns(1);
            ImGui::EndChild();ImGui::Spacing();
            ImGui::BeginChild("##demollog",ImVec2(W,0),true);
            if(g.demols.empty()){ImGui::TextDisabled("%s",L().noDemolYet);}
            else{
                ImGui::Columns(4,"dl",true);
                ImGui::SetColumnWidth(0,90);ImGui::SetColumnWidth(1,70);
                ImGui::SetColumnWidth(2,70);ImGui::SetColumnWidth(3,90);
                ImGui::TextDisabled("%s",L().playerCol);ImGui::NextColumn();
                ImGui::TextDisabled("X");ImGui::NextColumn();
                ImGui::TextDisabled("Z");ImGui::NextColumn();
                ImGui::TextDisabled("%s",L().whenCol);ImGui::NextColumn();
                ImGui::Separator();
                ImGuiListClipper dclip;
                dclip.Begin((int)g.demols.size());
                while(dclip.Step()){
                    for(int i=dclip.DisplayEnd-1;i>=dclip.DisplayStart;i--){
                        auto& d=g.demols[i];
                        ImGui::Text("%s",d.player.c_str());ImGui::NextColumn();
                        ImGui::Text("%.0f",d.x);ImGui::NextColumn();
                        ImGui::Text("%.0f",d.z);ImGui::NextColumn();
                        ImGui::TextDisabled("%s",FormatTime(d.ts).c_str());ImGui::NextColumn();
                    }
                }
                dclip.End();
                ImGui::Columns(1);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void TabMap()
{
    float W=ImGui::GetContentRegionAvail().x;
    float H=ImGui::GetContentRegionAvail().y;

    ImGui::BeginChild("##mapctrl",ImVec2(W,32),false);
    ImGui::Text("Zoom:");ImGui::SameLine();ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("##zoom",&g.mapZoom,0.5f,4.f,"%.1fx");
    ImGui::SameLine(0,16);
    if(ImGui::Button("Reset",ImVec2(60,22))){g.mapZoom=1.f;g.mapOffX=0;g.mapOffZ=0;}
    ImGui::SameLine(0,16);
    ImGui::TextDisabled("Builds:%d Demos:%d",(int)g.builds.size(),(int)g.demols.size());
    ImGui::EndChild();

    ImVec2 cpos=ImGui::GetCursorScreenPos();
    ImVec2 csz=ImVec2(W,H-36);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(cpos,ImVec2(cpos.x+csz.x,cpos.y+csz.y),IM_COL32(14,22,14,255));
    dl->AddRect(cpos,ImVec2(cpos.x+csz.x,cpos.y+csz.y),IM_COL32(45,70,45,200));
    for(int i=1;i<8;i++){
        float gx=cpos.x+csz.x*i/8,gy=cpos.y+csz.y*i/8;
        dl->AddLine(ImVec2(gx,cpos.y),ImVec2(gx,cpos.y+csz.y),IM_COL32(30,45,30,180));
        dl->AddLine(ImVec2(cpos.x,gy),ImVec2(cpos.x+csz.x,gy),IM_COL32(30,45,30,180));
    }

    if(!g.builds.empty()||!g.demols.empty()){
        UpdateMapBounds();
        float rX=g.mapMaxX-g.mapMinX;if(rX<500)rX=500;
        float rZ=g.mapMaxZ-g.mapMinZ;if(rZ<500)rZ=500;
        float pad=24.f;
        float scaleX=(csz.x-pad*2)*g.mapZoom/rX;
        float scaleZ=(csz.y-pad*2)*g.mapZoom/rZ;

        if(g.showTerr){
            for(auto& p:g.players){
                if(!p.hasTerr)continue;
                float px1=cpos.x+pad+(p.tx1-g.mapMinX)*scaleX+g.mapOffX;
                float pz1=cpos.y+pad+(p.tz1-g.mapMinZ)*scaleZ+g.mapOffZ;
                float px2=cpos.x+pad+(p.tx2-g.mapMinX)*scaleX+g.mapOffX;
                float pz2=cpos.y+pad+(p.tz2-g.mapMinZ)*scaleZ+g.mapOffZ;
                ImU32 tc=p.terrColor?p.terrColor:IM_COL32(200,100,50,40);
                dl->AddRectFilled(ImVec2(px1,pz1),ImVec2(px2,pz2),tc);
                dl->AddRect(ImVec2(px1,pz1),ImVec2(px2,pz2),tc|0xFF000000);
            }
        }

        for(auto& b:g.builds){
            float px=cpos.x+pad+(b.x-g.mapMinX)*scaleX+g.mapOffX;
            float pz=cpos.y+pad+(b.z-g.mapMinZ)*scaleZ+g.mapOffZ;
            if(px<cpos.x||px>cpos.x+csz.x||pz<cpos.y||pz>cpos.y+csz.y)continue;
            int ci=0;
            for(int pi=0;pi<(int)g.players.size();pi++)
                if(g.players[pi].name==b.player){ci=pi%4;break;}
            dl->AddCircleFilled(ImVec2(px,pz),4.f,g_playerColorsU32[ci]);
            dl->AddCircle(ImVec2(px,pz),4.f,IM_COL32(255,255,255,80));
        }

        if(g.showDemoOnMap){
            for(auto& d:g.demols){
                float px=cpos.x+pad+(d.x-g.mapMinX)*scaleX+g.mapOffX;
                float pz=cpos.y+pad+(d.z-g.mapMinZ)*scaleZ+g.mapOffZ;
                if(px<cpos.x||px>cpos.x+csz.x||pz<cpos.y||pz>cpos.y+csz.y)continue;
                dl->AddLine(ImVec2(px-4,pz-4),ImVec2(px+4,pz+4),IM_COL32(255,80,40,200));
                dl->AddLine(ImVec2(px+4,pz-4),ImVec2(px-4,pz+4),IM_COL32(255,80,40,200));
            }
        }
    } else {
        const char* msg="No data yet";
        ImVec2 ts=ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(cpos.x+(csz.x-ts.x)*0.5f,cpos.y+(csz.y-ts.y)*0.5f),
                    IM_COL32(60,90,60,255),msg);
    }

    ImGui::InvisibleButton("##mapbtn",csz);
    if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(0)){
        ImVec2 d=ImGui::GetIO().MouseDelta;
        g.mapOffX+=d.x; g.mapOffZ+=d.y;
    }
    if(ImGui::IsItemHovered()){
        float w=ImGui::GetIO().MouseWheel;
        if(w!=0){g.mapZoom+=w*0.15f;g.mapZoom=std::max(0.3f,std::min(8.f,g.mapZoom));}
    }
}

static void TabStats()
{
    float W=ImGui::GetContentRegionAvail().x;
    float half=(W-6)*0.5f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.10f,0.10f,0.14f,1.f));
    ImGui::BeginChild("##lang",ImVec2(W,36),true);
    ImGui::Text("%s:",L().langHdr);ImGui::SameLine(80);
    if(ImGui::RadioButton("English",g_lang==LANG_EN)){g_lang=LANG_EN;SaveLanguage();}
    ImGui::SameLine(0,16);
    if(ImGui::RadioButton("Ru",g_lang==LANG_RU)){g_lang=LANG_RU;SaveLanguage();}
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::Spacing();

    ImGui::BeginChild("##sess",ImVec2(half,160),true);
    SectionHeader(L().sessionHdr);
    ImGui::Text("%s",L().pluginRow);ImGui::SameLine();
    ImGui::TextColored(g_pluginFound?ImVec4(0.3f,0.9f,0.3f,1.f):ImVec4(0.8f,0.3f,0.3f,1.f),
                       g_pluginFound?"Found":"Not found");
    ImGui::Text("%s",L().networkRow);ImGui::SameLine();
    ImGui::TextColored(g_networkOnline?ImVec4(0.3f,0.9f,0.3f,1.f):ImVec4(0.8f,0.3f,0.3f,1.f),
                       g_networkOnline?"Online":"Offline");
    ImGui::Text("%s",L().durationRow);ImGui::SameLine();
    {std::string dur=SessionDuration();ImGui::Text("%s",dur.empty()?"--":dur.c_str());}
    ImGui::Text("%s",L().pingRow);ImGui::SameLine();
    if(g_networkOnline)DrawPingBar(g.ping);else ImGui::TextDisabled("--");
    ImGui::Text("%s",L().modeRow);ImGui::SameLine();
    ImGui::Text("%s",g.isCoopMode?L().coopStr:L().terrStr);
    if(g_myZeroTierIP[0]){
        ImGui::Text("%s",L().zerotierRow);ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.f),"%s",g_myZeroTierIP);
    }
    if(g_shmUptime>0){ImGui::TextDisabled("Uptime: %us",g_shmUptime);}
    ImGui::EndChild();ImGui::SameLine(0,6);

    ImGui::BeginChild("##act",ImVec2(half,160),true);
    SectionHeader(L().activityHdr);
    ImGui::Text("%s",L().buildingsRow);ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.builds.size());
    ImGui::Text("%s",L().demolRow);ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.demols.size());
    ImGui::Text("%s",L().chatRow);ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",g.totalMessages);
    ImGui::Text("%s",L().dealsRow);ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.deals.size());
    ImGui::Text("%s",L().playersRow);ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.players.size());

    if(!g.pingHistory.empty()){
        ImGui::Spacing();
        char ovl[32]; snprintf(ovl,32,"%.0fms",g.pingHistory.back());
        ImGui::PlotLines("##pg",g.pingHistory.data(),(int)g.pingHistory.size(),
                         0,ovl,0,500.f,ImVec2(half-20,50));
    }
    ImGui::EndChild();ImGui::Spacing();

    ImGui::BeginChild("##pstats",ImVec2(W,100),true);
    SectionHeader(L().playerStatsHdr);
    if(g.players.empty()){ImGui::TextDisabled("%s",L().noPlayersConn);}
    else{
        ImGui::Columns(4,"ps",true);
        ImGui::SetColumnWidth(0,120);ImGui::SetColumnWidth(1,60);ImGui::SetColumnWidth(2,50);
        ImGui::TextDisabled("%s",L().playerCol);ImGui::NextColumn();
        ImGui::TextDisabled("Ping");ImGui::NextColumn();
        ImGui::TextDisabled("Builds");ImGui::NextColumn();
        ImGui::TextDisabled("Demos");ImGui::NextColumn();
        ImGui::Separator();
        for(auto& p:g.players){
            ImGui::TextColored(p.color,"%s",p.name.c_str());ImGui::NextColumn();
            DrawPingBar(p.ping);ImGui::NextColumn();
            ImGui::Text("%d",p.builds);ImGui::NextColumn();
            ImGui::Text("%d",p.demolishes);ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();ImGui::Spacing();

    ImGui::BeginChild("##clog",ImVec2(W,0),true);
    SectionHeader(L().connLogHdr);
    if(ImGui::SmallButton(L().clearBtn))g.connLog.clear();
    ImGui::SameLine();
    if(ImGui::SmallButton(L().copyBtn)){
        std::string all;
        for(auto& e:g.connLog)all+=e.text+"\n";
        ImGui::SetClipboardText(all.c_str());
    }
    if(g.connLog.empty()){ImGui::TextDisabled("  --");}
    else{
        ImGuiListClipper cc;
        cc.Begin((int)g.connLog.size());
        while(cc.Step())
            for(int i=cc.DisplayEnd-1;i>=cc.DisplayStart;i--)
                ImGui::TextColored(g.connLog[i].color,"  %s",g.connLog[i].text.c_str());
        cc.End();
    }
    ImGui::EndChild();
}

static void TabAdmin()
{
    float W=ImGui::GetContentRegionAvail().x;
    SectionHeader(L().adminHdr);
    if(!g_networkOnline){ImGui::TextDisabled("%s",L().noAdminConn);return;}

    ImGui::Text("%s",L().adminSelectPlayer);ImGui::SameLine(0,8);
    ImGui::SetNextItemWidth(140);
    if(ImGui::BeginCombo("##at",g.adminTarget[0]?g.adminTarget:"--")){
        for(auto& p:g.players)
            if(ImGui::Selectable(p.name.c_str()))strncpy(g.adminTarget,p.name.c_str(),63);
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::Text("%s",L().adminReasonLabel);ImGui::SameLine();
    ImGui::SetNextItemWidth(W-120);ImGui::InputText("##ar",g.adminReason,128);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.7f,0.15f,0.08f,1.f));
    if(ImGui::Button(L().adminKick,ImVec2(80,26)))DoAdminAction(ADMIN_KICK);
    ImGui::PopStyleColor();
    ImGui::SameLine(0,6);
    if(ImGui::Button(L().adminMute,ImVec2(70,26)))DoAdminAction(ADMIN_MUTE);
    ImGui::SameLine(0,6);
    if(ImGui::Button(L().adminUnmute,ImVec2(80,26)))DoAdminAction(ADMIN_UNMUTE);
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();

    SectionHeader(L().terrHdr);
    ImGui::Text("X1:");ImGui::SameLine();ImGui::SetNextItemWidth(70);ImGui::InputFloat("##tx1",&g.terrX1,0,0,"%.0f");
    ImGui::SameLine(0,8);ImGui::Text("Z1:");ImGui::SameLine();ImGui::SetNextItemWidth(70);ImGui::InputFloat("##tz1",&g.terrZ1,0,0,"%.0f");
    ImGui::Text("X2:");ImGui::SameLine();ImGui::SetNextItemWidth(70);ImGui::InputFloat("##tx2",&g.terrX2,0,0,"%.0f");
    ImGui::SameLine(0,8);ImGui::Text("Z2:");ImGui::SameLine();ImGui::SetNextItemWidth(70);ImGui::InputFloat("##tz2",&g.terrZ2,0,0,"%.0f");
    if(ImGui::Button(L().terrSetBtn,ImVec2(130,24)))DoSetTerritory(false);
    ImGui::SameLine(0,6);
    if(ImGui::Button(L().terrClearBtn,ImVec2(130,24)))DoSetTerritory(true);
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();

    ImGui::Text("%s",L().adminMsgLabel);ImGui::SameLine();
    ImGui::SetNextItemWidth(W-160);ImGui::InputText("##am",g.adminMsg,256);
    ImGui::SameLine(0,6);
    if(ImGui::Button(L().adminBroadcast,ImVec2(90,22)))DoAdminBroadcast();
}

static void TabSettings()
{
    float W=ImGui::GetContentRegionAvail().x;
    SectionHeader(L().settingsHdr);

    ImGui::Text("%s",L().nameLabel);ImGui::SameLine(100);ImGui::SetNextItemWidth(W-110);ImGui::InputText("##sn",g.name,64);
    ImGui::Text("Default IP:");ImGui::SameLine(100);ImGui::SetNextItemWidth(W-110);ImGui::InputText("##si",g.ip,64);
    ImGui::Text("%s",L().portLabel);ImGui::SameLine(100);ImGui::SetNextItemWidth(60);ImGui::InputText("##sp",g.port,8);
    ImGui::Spacing();
    ImGui::Checkbox(L().showBuildNotif,&g.showBuildNotif);
    ImGui::Checkbox(L().autoscrollChat,&g.autoscrollChat);
    ImGui::Checkbox(L().showDemoOnMap,&g.showDemoOnMap);
    ImGui::Checkbox(L().showTerr,&g.showTerr);
    ImGui::Spacing();
    if(ImGui::Button("Save",ImVec2(100,24)))SaveSettings(g.name,g.ip,g.port);
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();

    SectionHeader(L().savedServersHdr);
    if(g.savedServerCount==0){ImGui::TextDisabled("%s",L().noSavedServers);}
    else{
        for(int i=0;i<g.savedServerCount;i++){
            auto& s=g.savedServers[i];
            ImGui::PushID(i);
            ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.f),"%s:%s",s.ip,s.port);
            ImGui::SameLine(0,8);ImGui::TextDisabled("(%s)",s.name);
            ImGui::SameLine(0,8);ImGui::TextDisabled("[%s]",s.lastSeen);
            ImGui::SameLine(0,8);
            if(ImGui::SmallButton(L().connectBtn)){
                strncpy(g.ip,s.ip,63);strncpy(g.port,s.port,7);
                g.isHost=false;DoConnect();
            }
            ImGui::SameLine(0,4);
            if(ImGui::SmallButton(L().deleteBtn))DeleteServer(i);
            ImGui::PopID();
        }
    }
}

static bool CreateDX11(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd={};
    sd.BufferCount=2;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hwnd;sd.SampleDesc.Count=1;sd.Windowed=TRUE;
    sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,
        NULL,0,NULL,0,D3D11_SDK_VERSION,&sd,&g_chain,&g_dev,&fl,&g_ctx)))return false;
    ID3D11Texture2D* bb=NULL;
    g_chain->GetBuffer(0,IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb,NULL,&g_rtv);
    bb->Release();return true;
}

static void CleanDX11()
{
    if(g_rtv){g_rtv->Release();g_rtv=NULL;}
    if(g_chain){g_chain->Release();g_chain=NULL;}
    if(g_ctx){g_ctx->Release();g_ctx=NULL;}
    if(g_dev){g_dev->Release();g_dev=NULL;}
}

static void ResizeDX11(UINT w, UINT h)
{
    if(g_rtv){g_rtv->Release();g_rtv=NULL;}
    g_chain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0);
    ID3D11Texture2D* bb=NULL;
    g_chain->GetBuffer(0,IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb,NULL,&g_rtv);
    bb->Release();
}

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    if(ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp))return true;
    switch(msg){
    case WM_SIZE:if(g_dev&&wp!=SIZE_MINIMIZED)ResizeDX11(LOWORD(lp),HIWORD(lp));return 0;
    case WM_SYSCOMMAND:if((wp&0xfff0)==SC_KEYMENU)return 0;break;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int)
{
    timeBeginPeriod(1);
    InitializeCriticalSection(&g.sendLock);
    WSADATA wsa;WSAStartup(MAKEWORD(2,2),&wsa);
    g_lang = LANG_EN;
    LoadSettings(g.name,g.ip,g.port);

    WNDCLASSEXA wc={sizeof(wc)};
    wc.style=CS_CLASSDC;wc.lpfnWndProc=WndProc;wc.hInstance=hInst;
    wc.lpszClassName="WRSRMp";
    wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    RegisterClassExA(&wc);

    g_hwnd=CreateWindowA("WRSRMp",L().windowTitle,
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,340,720,
        NULL,NULL,hInst,NULL);

    if(!CreateDX11(g_hwnd)){CleanDX11();return 1;}

    IMGUI_CHECKVERSION();ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename=NULL;

    static const ImWchar ranges[]={0x0020,0x00FF,0x0400,0x044F,0};
    if(!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf",14.f,NULL,ranges))
        io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev,g_ctx);
    ApplyTheme();
    LoadResources();
    FilterCat(0);
    ShowWindow(g_hwnd,SW_SHOW);UpdateWindow(g_hwnd);

    const float CC[4]={0.08f,0.08f,0.10f,1.f};
    MSG msg={};
    ULONGLONG lastFrame=GetTickCount64();

    while(g_running){
        ULONGLONG now=GetTickCount64();
        bool focused=(GetForegroundWindow()==g_hwnd);
        ULONGLONG frameMs=focused?FRAME_MS:FRAME_MS_UNFOCUSED;
        if(now-lastFrame<frameMs){Sleep(1);continue;}
        lastFrame=GetTickCount64();

        while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){
            TranslateMessage(&msg);DispatchMessageA(&msg);
            if(msg.message==WM_QUIT)g_running=false;
        }
        if(!g_running)break;

        if(IsIconic(g_hwnd)){Sleep(100);continue;}

        PollSharedMemory();

        if(g_networkOnline&&GetTickCount64()-g.lastPingTime>5000){
            g.lastPingTime=GetTickCount64();
            NetSendMsg(g.sock,MSG_PING,nullptr,0);
        }

        ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
        ImGuiIO& io2=ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io2.DisplaySize);
        ImGui::Begin("##root",NULL,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);

        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.88f,0.28f,0.18f,1.f));
        ImGui::Text("  WRSR Multiplayer");ImGui::PopStyleColor();
        ImGui::SameLine();ImGui::TextDisabled("v0.8");
        ImGui::SameLine(io2.DisplaySize.x-100);
        if(g_networkOnline)
            ImGui::TextColored(ImVec4(0.3f,0.85f,0.3f,1.f),"%s",L().statusOnline);
        else if(g_pluginFound)
            ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.f),"%s",L().statusStandby);
        else
            ImGui::TextDisabled("%s",L().statusOffline);
        ImGui::Separator();

        if(ImGui::BeginTabBar("##tabs")){
            if(ImGui::BeginTabItem(L().tabMain))     {TabMain();     ImGui::EndTabItem();}
            if(ImGui::BeginTabItem(L().tabImport))   {TabImport();   ImGui::EndTabItem();}
            if(ImGui::BeginTabItem(L().tabBuildings)){TabBuildings();ImGui::EndTabItem();}
            if(ImGui::BeginTabItem(L().tabMap))      {TabMap();      ImGui::EndTabItem();}
            if(ImGui::BeginTabItem(L().tabStats))    {TabStats();    ImGui::EndTabItem();}
            if(ImGui::BeginTabItem(L().tabAdmin))    {TabAdmin();    ImGui::EndTabItem();}
            if(ImGui::BeginTabItem(L().tabSettings)) {TabSettings(); ImGui::EndTabItem();}
            ImGui::EndTabBar();
        }

        double t=ImGui::GetTime();float ny=io2.DisplaySize.y-16;
        for(int i=(int)g.notifications.size()-1;i>=0;i--){
            auto& n=g.notifications[i];
            double e2=t-n.ts;
            if(e2>3.5){g.notifications.erase(g.notifications.begin()+i);continue;}
            float alpha=(float)(e2>2.5?(3.5-e2):1.0);
            ny-=28;
            ImGui::SetNextWindowBgAlpha(0.82f*alpha);
            ImGui::SetNextWindowPos(ImVec2(io2.DisplaySize.x*0.5f,ny),
                                    ImGuiCond_Always,ImVec2(0.5f,0.f));
            char wid[16];snprintf(wid,16,"##n%d",i);
            ImGui::Begin(wid,NULL,
                ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
                ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
                ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoMove);
            ImGui::TextColored(ImVec4(n.color.x,n.color.y,n.color.z,alpha),
                               "  %s  ",n.text.c_str());
            ImGui::End();
        }

        ImGui::End();ImGui::Render();
        g_ctx->OMSetRenderTargets(1,&g_rtv,NULL);
        g_ctx->ClearRenderTargetView(g_rtv,CC);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_chain->Present(1,0);
    }

    DoDisconnect();
    ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
    CleanDX11();DestroyWindow(g_hwnd);UnregisterClassA("WRSRMp",hInst);
    g_shm.Destroy();WSACleanup();DeleteCriticalSection(&g.sendLock);
    timeEndPeriod(1);
    return 0;
}