#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
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

#ifdef __cplusplus

#endif

#pragma comment(lib,"ws2_32.lib")
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

struct MsgHeader { BYTE type; DWORD size; };

struct BuildCmd {
    float x, z;
    char  typeName[128];
    char  playerName[64];
    float rotation;
};

struct ResourceReq {
    char fromPlayer[64];
    char toPlayer[64];
    char resource[64];
    int  amount;
    int  price;
    BYTE accepted;
};

struct PatchStream { const char* buf; size_t pos; size_t size; };

enum ChatType { CHAT_NORMAL, CHAT_SYSTEM, CHAT_BUILD, CHAT_TRADE, CHAT_ERROR };

struct ChatMsg {
    std::string text;
    ChatType    type;
    double      timestamp;
};

struct PlayerInfo {
    std::string name;
    DWORD       ping;
    bool        connected;
    ImVec4      color;
    int         builds;
    float       territory_x1, territory_z1;
    float       territory_x2, territory_z2;
    bool        hasTerr;
};

struct BuildLog {
    std::string player;
    std::string type;
    float x, z;
    double timestamp;
};

struct ResourceDeal {
    std::string from;
    std::string to;
    std::string resource;
    int         amount;
    int         price;
    bool        pending;
    bool        accepted;
    double      timestamp;
};

struct Notification {
    std::string text;
    ImVec4      color;
    double      timestamp;
    float       alpha;
};

struct ResCat { const char* name; const char* kw[20]; };
static ResCat g_cats[] = {
    {"Raw Materials",   {"rawcoal","rawgravel","rawiron","rawbauxite","rawgold","rawcopper","rawuranium",NULL}},
    {"Fuel & Energy",   {"coal","oil","fuel","nuclearfuel","nuclearfuelburned","yellowcake","uranium","heat","eletric","gas",NULL}},
    {"Construction",    {"bricks","boards","cement","steel","prefabpanels","asphalt","gravel","wood","concrete","sand","glass",NULL}},
    {"Industry",        {"iron","aluminium","alumina","bauxite","plastics","chemicals","explosives","ecomponents","mcomponents","eletronics","fabric","rubber","bitumen",NULL}},
    {"Agriculture",     {"food","meat","livestock","fertiliser","fertiliser_liquid","plants","alcohol",NULL}},
    {"Liquids",         {"water","colorwater","usagewater",NULL}},
    {"Waste",           {"waste",NULL}},
    {"Cargo",           {"container_big","container_small","crate","generalcargo",NULL}},
    {"Nuclear",         {"uranium","yellowcake","nuclearfuel","nuclearfuelburned",NULL}},
    {"Other",           {NULL}},
};
static int g_catCount = 10;

static ImVec4 g_playerColors[] = {
    ImVec4(0.90f,0.30f,0.20f,1.f),
    ImVec4(0.20f,0.60f,0.90f,1.f),
    ImVec4(0.20f,0.80f,0.35f,1.f),
    ImVec4(0.90f,0.70f,0.10f,1.f),
};

static SharedMemory g_shm;
static bool g_shmConnected = false;

struct AppState {

    char ip[64]   = "127.0.0.1";
    char port[8]  = "7777";
    char name[64] = "Player1";
    bool isHost   = true;
    bool isCoopMode = true;

    SOCKET        sock        = INVALID_SOCKET;
    HANDLE        recvThread  = NULL;
    CRITICAL_SECTION sendLock;
    ULONGLONG     lastPingTime = 0;
    DWORD         ping         = 0;
    bool          connected    = false;
    std::string   statusText   = "Not connected";

    int  progressCur = 0;
    int  progressMax = 0;
    bool syncing     = false;
    std::string syncStatus;

    std::vector<PlayerInfo>   players;
    std::vector<ChatMsg>      chat;
    std::vector<BuildLog>     builds;
    std::vector<ResourceDeal> deals;
    std::vector<Notification> notifications;
    std::vector<std::string>  allResources;
    std::vector<std::string>  catResources;

    int   tab          = 0;
    char  chatInput[256] = {};
    int   selectedCat  = 0;
    int   selectedRes  = -1;
    int   selectedDeal = -1;
    char  amountBuf[16] = "100";
    char  priceBuf[16]  = "0";
    char  buildFilter[64] = {};
    char  resFilter[64]   = {};
    float mapZoom   = 1.0f;
    float mapOffX   = 0.0f;
    float mapOffZ   = 0.0f;
    bool  mapDragging = false;
    bool  showSettings = false;
    bool  showAbout    = false;

    int totalBuilds   = 0;
    int totalMessages = 0;
    int totalDeals    = 0;
    double sessionStart = 0;
} g;

static ID3D11Device*           g_dev    = NULL;
static ID3D11DeviceContext*    g_ctx    = NULL;
static IDXGISwapChain*         g_chain  = NULL;
static ID3D11RenderTargetView* g_rtv    = NULL;
static HWND                    g_hwnd   = NULL;
static bool                    g_running = true;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static bool NetSend(SOCKET s,const char* b,int n)
{
    int sent=0;
    while(sent<n){int r=send(s,b+sent,n-sent,0);if(r<=0)return false;sent+=r;}
    return true;
}
static bool NetRecv(SOCKET s,char* b,int n)
{
    int got=0;
    while(got<n){int r=recv(s,b+got,n-got,0);if(r<=0)return false;got+=r;}
    return true;
}
static void NetDrain(SOCKET s,DWORD n)
{
    char t[512];
    while(n>0){DWORD k=n>512?512:n;if(!NetRecv(s,t,k))break;n-=k;}
}
static void NetSendMsg(SOCKET s,BYTE type,const void* data,DWORD sz)
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

static char* FileRead(const char* p,DWORD* sz)
{
    HANDLE h=CreateFileA(p,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE){*sz=0;return nullptr;}
    *sz=GetFileSize(h,NULL);
    char* b=(char*)malloc(*sz);if(!b){CloseHandle(h);*sz=0;return nullptr;}
    DWORD rd;ReadFile(h,b,*sz,&rd,NULL);CloseHandle(h);return b;
}
static void FileWrite(const char* p,const char* d,DWORD sz)
{
    HANDLE h=CreateFileA(p,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(h==INVALID_HANDLE_VALUE)return;
    DWORD wr;WriteFile(h,d,sz,&wr,NULL);CloseHandle(h);
}
static int PatchRead(const struct bspatch_stream* s,void* b,int n)
{
    PatchStream* ps=(PatchStream*)s->opaque;
    if(ps->pos+n>ps->size)return -1;
    memcpy(b,ps->buf+ps->pos,n);ps->pos+=n;return 0;
}
static void RecvFull(SOCKET s,const char* path)
{
    DWORD oSz=0,cSz=0;
    if(!NetRecv(s,(char*)&oSz,4)||!NetRecv(s,(char*)&cSz,4))return;
    if(!oSz||!cSz)return;
    char* cb=(char*)malloc(cSz);if(!cb)return;
    NetRecv(s,cb,cSz);
    char* ob=(char*)malloc(oSz);if(!ob){free(cb);return;}
    size_t r=ZSTD_decompress(ob,oSz,cb,cSz);free(cb);
    if(!ZSTD_isError(r))FileWrite(path,ob,(DWORD)r);free(ob);
}
static void RecvDiff(SOCKET s,const char* path)
{
    DWORD nSz=0,rSz=0,cSz=0;
    if(!NetRecv(s,(char*)&nSz,4)||!NetRecv(s,(char*)&rSz,4)||!NetRecv(s,(char*)&cSz,4))return;
    if(!nSz||!rSz||!cSz)return;
    char* cd=(char*)malloc(cSz);if(!cd)return;NetRecv(s,cd,cSz);
    char* rd=(char*)malloc(rSz);if(!rd){free(cd);return;}
    size_t dr=ZSTD_decompress(rd,rSz,cd,cSz);free(cd);
    if(ZSTD_isError(dr)){free(rd);return;}
    DWORD oSz=0;char* od=FileRead(path,&oSz);
    if(!od){free(rd);return;}
    char* nd=(char*)malloc(nSz);if(!nd){free(rd);free(od);return;}
    PatchStream ps={rd,0,rSz};
    struct bspatch_stream bs={};bs.opaque=&ps;bs.read=PatchRead;
    if(bspatch((uint8_t*)od,oSz,(uint8_t*)nd,nSz,&bs)==0)FileWrite(path,nd,nSz);
    free(rd);free(od);free(nd);
}

static bool ResInCat(const std::string& nm,int cat)
{
    if(cat==g_catCount-1){
        for(int c=0;c<g_catCount-1;c++)if(ResInCat(nm,c))return false;
        return true;
    }
    for(int i=0;g_cats[cat].kw[i];i++)
        if(nm.find(g_cats[cat].kw[i])==0)return true;
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
    static const char* skip[]={
        "ships","airplanes","helicopters","trains","vehicles",
        "workers","haldauhlealpha","service_material","mcomponents",
        "ecomponents",NULL};
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
    g.catResources.clear();
    g.selectedRes=-1;
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
    ChatMsg m; m.text=text; m.type=type;
    m.timestamp=ImGui::GetTime();
    g.chat.push_back(m);
    g.totalMessages++;
    if(g.chat.size()>500)g.chat.erase(g.chat.begin());
}
static void AddNotif(const std::string& text,
                     ImVec4 col=ImVec4(0.9f,0.8f,0.2f,1.f))
{
    Notification n; n.text=text; n.color=col;
    n.timestamp=ImGui::GetTime(); n.alpha=1.f;
    g.notifications.push_back(n);
    if(g.notifications.size()>5)
        g.notifications.erase(g.notifications.begin());
}

static DWORD WINAPI RecvThread(LPVOID)
{
    while(g.connected){
        MsgHeader hdr;
        if(!NetRecv(g.sock,(char*)&hdr,sizeof(hdr))){
            g.connected=false;
            g.statusText="Disconnected";
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
            for(auto& p:g.players)
                if(p.name==g.name)p.ping=g.ping;
            break;
        case MSG_BUILD:
            if(hdr.size==sizeof(BuildCmd)){
                BuildCmd c; NetRecv(g.sock,(char*)&c,sizeof(c));
                BuildLog bl;
                bl.player=c.playerName; bl.type=c.typeName;
                bl.x=c.x; bl.z=c.z;
                bl.timestamp=ImGui::GetTime();
                g.builds.push_back(bl);
                g.totalBuilds++;

                for(auto& p:g.players)
                    if(p.name==c.playerName)p.builds++;
                char buf[256];
                snprintf(buf,256,"[BUILD] %s placed %s at (%.0f, %.0f)",
                         c.playerName,c.typeName,c.x,c.z);
                AddChat(buf,CHAT_BUILD);
                AddNotif(std::string(c.playerName)+" built "+c.typeName,
                         ImVec4(0.4f,0.85f,0.4f,1.f));
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_RESOURCE_REQ:
            if(hdr.size==sizeof(ResourceReq)){
                ResourceReq r; NetRecv(g.sock,(char*)&r,sizeof(r));
                ResourceDeal d;
                d.from=r.fromPlayer; d.to=r.toPlayer;
                d.resource=r.resource; d.amount=r.amount;
                d.price=r.price; d.pending=true; d.accepted=false;
                d.timestamp=ImGui::GetTime();
                g.deals.push_back(d);
                char buf[256];
                snprintf(buf,256,"[TRADE] %s requests %d x %s (price: %d)",
                         r.fromPlayer,r.amount,r.resource,r.price);
                AddChat(buf,CHAT_TRADE);
                snprintf(buf,256,"%s wants %d x %s",
                         r.fromPlayer,r.amount,r.resource);
                AddNotif(buf,ImVec4(0.9f,0.7f,0.1f,1.f));
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_CHAT:
            if(hdr.size>0&&hdr.size<4096){
                std::string s(hdr.size,'\0');
                NetRecv(g.sock,&s[0],hdr.size);
                AddChat(s,CHAT_NORMAL);
            } else NetDrain(g.sock,hdr.size);
            break;
        case MSG_SAVE_FULL:
        case MSG_SAVE_DIFF:{
            bool diff=(hdr.type==MSG_SAVE_DIFF);
            int fc=0; NetRecv(g.sock,(char*)&fc,4);
            g.syncing=true; g.progressMax=fc; g.progressCur=0;
            g.syncStatus=diff?"Receiving delta update...":"Receiving full save...";
            SHCreateDirectoryExA(NULL,g_saveDir,NULL);
            for(int i=0;i<fc;i++){
                int nl=0; NetRecv(g.sock,(char*)&nl,4);
                char fn[MAX_PATH]={};
                int rl=nl<MAX_PATH-1?nl:MAX_PATH-1;
                NetRecv(g.sock,fn,rl);
                if(nl>rl)NetDrain(g.sock,nl-rl);
                char fp[MAX_PATH];
                snprintf(fp,MAX_PATH,"%s\\%s",g_saveDir,fn);
                if(diff)RecvDiff(g.sock,fp);
                else    RecvFull(g.sock,fp);
                g.progressCur=i+1;
            }
            g.syncing=false;
            g.syncStatus=diff?"Delta applied — reload save":"Done! Load the mp_client save";
            g.statusText=g.syncStatus;
            AddChat(std::string("[SYNC] ")+g.syncStatus,CHAT_SYSTEM);
            break;}
        default: NetDrain(g.sock,hdr.size); break;
        }
    }
    return 0;
}

static void DoConnect()
{
    if (g_shmConnected && g_shm.block) {
        g.statusText = "Connecting...";
        AddChat("[SYSTEM] Sending connect command to plugin...", CHAT_SYSTEM);
        g_shm.SendCommand(MP_CMD_CONNECT, g.ip, g.port, g.name);
        g.sessionStart = ImGui::GetTime();
        AddNotif("Connect command sent!", ImVec4(0.9f,0.7f,0.1f,1.f));
    } else {
        AddChat("[ERROR] Plugin not found — is the game running?", CHAT_ERROR);
        g.statusText = "Plugin not connected";
        AddNotif("Plugin not found!", ImVec4(0.9f,0.3f,0.2f,1.f));
    }
}
static void DoDisconnect()
{
    g.connected=false;
    KillTimer(g_hwnd,1);
    if(g.sock!=INVALID_SOCKET){closesocket(g.sock);g.sock=INVALID_SOCKET;}
    if(g.recvThread){
        WaitForSingleObject(g.recvThread,1000);
        CloseHandle(g.recvThread);g.recvThread=NULL;
    }
    g.players.clear();
    g.ping=0;
    g.statusText="Disconnected";
    AddChat("[SYSTEM] Disconnected",CHAT_SYSTEM);
}
static void DoSendChat()
{
    if(!g.chatInput[0])return;
    if (g_shmConnected && g_shm.block) {
        g_shm.SendCommand(MP_CMD_CHAT, g.chatInput);
        char full[320];
        snprintf(full,320,"[%s]: %s",g.name,g.chatInput);
        AddChat(full,CHAT_NORMAL);
    }
    g.chatInput[0]='\0';
    ImGui::SetKeyboardFocusHere(-1);
}
static void DoSendRequest()
{
    if(!g.connected||g.selectedRes<0||g.selectedRes>=(int)g.catResources.size())return;
    ResourceReq rq={};
    strncpy(rq.fromPlayer,g.name,63);
    strncpy(rq.resource,g.catResources[g.selectedRes].c_str(),63);
    rq.amount=atoi(g.amountBuf);
    rq.price=atoi(g.priceBuf);
    if(rq.amount<=0)return;
    NetSendMsg(g.sock,MSG_RESOURCE_REQ,&rq,sizeof(rq));
    ResourceDeal d;
    d.from=g.name; d.resource=rq.resource;
    d.amount=rq.amount; d.price=rq.price;
    d.pending=false; d.accepted=false;
    d.timestamp=ImGui::GetTime();
    g.deals.push_back(d);
    g.totalDeals++;
    char buf[256];
    snprintf(buf,256,"[TRADE] You requested %d x %s (price: %d)",
             rq.amount,rq.resource,rq.price);
    AddChat(buf,CHAT_TRADE);
    AddNotif("Request sent!",ImVec4(0.9f,0.7f,0.1f,1.f));
}

static void ApplyTheme()
{
    ImGuiStyle& s=ImGui::GetStyle();
    s.WindowRounding  = 0.f;
    s.ChildRounding   = 6.f;
    s.FrameRounding   = 4.f;
    s.GrabRounding    = 4.f;
    s.TabRounding     = 4.f;
    s.ScrollbarRounding=4.f;
    s.FramePadding    = ImVec2(8,5);
    s.ItemSpacing     = ImVec2(8,6);
    s.WindowPadding   = ImVec2(10,10);
    s.ScrollbarSize   = 11.f;
    s.GrabMinSize     = 8.f;
    s.WindowBorderSize= 0.f;
    s.ChildBorderSize = 1.f;

    ImVec4* c=s.Colors;
    c[ImGuiCol_WindowBg]          =ImVec4(0.11f,0.11f,0.14f,1.f);
    c[ImGuiCol_ChildBg]           =ImVec4(0.09f,0.09f,0.12f,1.f);
    c[ImGuiCol_PopupBg]           =ImVec4(0.12f,0.12f,0.16f,0.97f);
    c[ImGuiCol_Border]            =ImVec4(0.25f,0.25f,0.32f,0.60f);
    c[ImGuiCol_FrameBg]           =ImVec4(0.16f,0.16f,0.21f,1.f);
    c[ImGuiCol_FrameBgHovered]    =ImVec4(0.20f,0.20f,0.27f,1.f);
    c[ImGuiCol_FrameBgActive]     =ImVec4(0.24f,0.24f,0.32f,1.f);
    c[ImGuiCol_TitleBg]           =ImVec4(0.08f,0.08f,0.11f,1.f);
    c[ImGuiCol_TitleBgActive]     =ImVec4(0.55f,0.12f,0.08f,1.f);
    c[ImGuiCol_MenuBarBg]         =ImVec4(0.14f,0.14f,0.18f,1.f);
    c[ImGuiCol_ScrollbarBg]       =ImVec4(0.08f,0.08f,0.11f,1.f);
    c[ImGuiCol_ScrollbarGrab]     =ImVec4(0.30f,0.10f,0.07f,1.f);
    c[ImGuiCol_ScrollbarGrabHovered]=ImVec4(0.45f,0.14f,0.10f,1.f);
    c[ImGuiCol_ScrollbarGrabActive] =ImVec4(0.60f,0.18f,0.13f,1.f);
    c[ImGuiCol_CheckMark]         =ImVec4(0.90f,0.35f,0.22f,1.f);
    c[ImGuiCol_SliderGrab]        =ImVec4(0.75f,0.22f,0.14f,1.f);
    c[ImGuiCol_SliderGrabActive]  =ImVec4(0.90f,0.28f,0.18f,1.f);
    c[ImGuiCol_Button]            =ImVec4(0.58f,0.14f,0.09f,1.f);
    c[ImGuiCol_ButtonHovered]     =ImVec4(0.72f,0.19f,0.12f,1.f);
    c[ImGuiCol_ButtonActive]      =ImVec4(0.85f,0.25f,0.16f,1.f);
    c[ImGuiCol_Header]            =ImVec4(0.48f,0.12f,0.08f,1.f);
    c[ImGuiCol_HeaderHovered]     =ImVec4(0.62f,0.16f,0.11f,1.f);
    c[ImGuiCol_HeaderActive]      =ImVec4(0.75f,0.20f,0.14f,1.f);
    c[ImGuiCol_Separator]         =ImVec4(0.25f,0.25f,0.32f,1.f);
    c[ImGuiCol_Tab]               =ImVec4(0.14f,0.14f,0.19f,1.f);
    c[ImGuiCol_TabHovered]        =ImVec4(0.68f,0.18f,0.12f,1.f);
    c[ImGuiCol_TabActive]         =ImVec4(0.62f,0.15f,0.10f,1.f);
    c[ImGuiCol_TabUnfocused]      =ImVec4(0.10f,0.10f,0.14f,1.f);
    c[ImGuiCol_TabUnfocusedActive]=ImVec4(0.38f,0.10f,0.07f,1.f);
    c[ImGuiCol_Text]              =ImVec4(0.90f,0.88f,0.85f,1.f);
    c[ImGuiCol_TextDisabled]      =ImVec4(0.48f,0.46f,0.44f,1.f);
    c[ImGuiCol_PlotLines]         =ImVec4(0.75f,0.25f,0.15f,1.f);
    c[ImGuiCol_PlotHistogram]     =ImVec4(0.75f,0.25f,0.15f,1.f);
}

static ImVec4 ChatColor(ChatType t)
{
    switch(t){
    case CHAT_SYSTEM: return ImVec4(0.5f,0.8f,0.5f,1.f);
    case CHAT_BUILD:  return ImVec4(0.4f,0.75f,0.95f,1.f);
    case CHAT_TRADE:  return ImVec4(0.95f,0.75f,0.25f,1.f);
    case CHAT_ERROR:  return ImVec4(0.95f,0.3f,0.3f,1.f);
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
static void PingBar(DWORD ping, float width=40.f)
{

    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 p=ImGui::GetCursorScreenPos();
    float barW=6.f, gap=2.f;
    int bars = ping==0?0 : ping<50?4 : ping<100?3 : ping<200?2 : 1;
    for(int i=0;i<4;i++){
        float h=4.f+(i*3.f);
        float x=p.x+i*(barW+gap);
        float y=p.y+16.f-h;
        ImU32 col = i<bars ? IM_COL32(80,200,80,255) : IM_COL32(60,60,70,255);
        dl->AddRectFilled(ImVec2(x,y),ImVec2(x+barW,p.y+16.f),col,1.f);
    }
    ImGui::Dummy(ImVec2(width,18.f));
}
static std::string FormatTime(double t)
{
    double elapsed=ImGui::GetTime()-t;
    char buf[32];
    if(elapsed<60) snprintf(buf,32,"%.0fs ago",elapsed);
    else if(elapsed<3600) snprintf(buf,32,"%.0fm ago",elapsed/60);
    else snprintf(buf,32,"%.0fh ago",elapsed/3600);
    return buf;
}
static std::string SessionDuration()
{
    if(!g.connected||g.sessionStart<=0)return "";
    double elapsed=ImGui::GetTime()-g.sessionStart;
    int h=(int)(elapsed/3600);
    int m=(int)(fmod(elapsed,3600)/60);
    int s=(int)fmod(elapsed,60);
    char buf[32];
    if(h>0) snprintf(buf,32,"%dh %dm",h,m);
    else     snprintf(buf,32,"%dm %ds",m,s);
    return buf;
}

static void PollSharedMemory()
{
    if (!g_shmConnected) {
        if (g_shm.Create(false)) {
            if (g_shm.block && g_shm.block->magic == SHARED_MAGIC) {
                g_shmConnected = true;
                AddChat("[SYSTEM] Plugin connected via shared memory", CHAT_SYSTEM);
                AddNotif("Plugin found!", ImVec4(0.3f,0.9f,0.3f,1.f));
            }
        }
        return;
    }
    if (!g_shm.block) return;
    if (!g_shm.Lock(50)) return;
    BYTE status = g_shm.block->status;
    std::string statusText = g_shm.block->statusText;
    DWORD playerCount = g_shm.block->playerCount;
    g.players.clear();
    for (DWORD i = 0; i < playerCount && i < 4; i++) {
        auto& ps = g_shm.block->players[i];
        if (!ps.connected) continue;
        PlayerInfo pi;
        pi.name = ps.name; pi.ping = ps.ping;
        pi.connected = true; pi.builds = 0; pi.hasTerr = false;
        pi.color = g_playerColors[i % 4];
        g.players.push_back(pi);
    }
    static DWORD s_lastBuild = 0;
    DWORD notifCount = g_shm.block->buildNotifyCount;
    if (notifCount > s_lastBuild) {
        for (DWORD i = s_lastBuild; i < notifCount; i++) {
            auto& bn = g_shm.block->buildNotify[i % MAX_BUILD_NOTIFY];
            BuildLog bl;
            bl.player = bn.playerName; bl.type = bn.typeName;
            bl.x = bn.x; bl.z = bn.z; bl.timestamp = ImGui::GetTime();
            g.builds.push_back(bl);
            char msg[256];
            snprintf(msg,256,"[BUILD] %s placed %s at (%.0f,%.0f)",
                     bn.playerName,bn.typeName,bn.x,bn.z);
            AddChat(msg, CHAT_BUILD);
        }
        s_lastBuild = notifCount;
    }
    g_shm.Unlock();
    g.statusText = statusText;
    g.connected = (status == MP_STATUS_CONNECTED || status == MP_STATUS_HOST);
}

static void TabMain()
{
    float W=ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.10f,0.10f,0.14f,1.f));
    float connH = g.connected ? 86.f : 148.f;
    ImGui::BeginChild("##conn",ImVec2(W,connH),true);

    bool ok=g.connected;
    ImU32 dotCol=ok?IM_COL32(60,210,80,255):IM_COL32(200,60,60,255);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 dp=ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(dp.x+7,dp.y+9),5.f,dotCol);
    ImGui::Dummy(ImVec2(16,0));ImGui::SameLine();
    ImGui::Text(ok?"Connected":"Not connected");
    if(ok){
        ImGui::SameLine(0,16);
        ImGui::TextDisabled("Ping: %dms",g.ping);
        ImGui::SameLine(0,16);
        std::string dur=SessionDuration();
        if(!dur.empty())ImGui::TextDisabled("Session: %s",dur.c_str());
    }

    if(!ok){
        ImGui::Spacing();

        ImGui::Text("Mode:");ImGui::SameLine(60);
        if(ImGui::RadioButton("Host",g.isHost)) g.isHost=true;
        ImGui::SameLine();
        if(ImGui::RadioButton("Client",!g.isHost)) g.isHost=false;
        ImGui::SameLine(0,20);
        ImGui::Text("Game:");ImGui::SameLine();
        if(ImGui::RadioButton("Coop",g.isCoopMode)) g.isCoopMode=true;
        ImGui::SameLine();
        if(ImGui::RadioButton("Terr",!g.isCoopMode)) g.isCoopMode=false;

        ImGui::SetNextItemWidth(175);
        ImGui::InputText("IP",g.ip,64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55);
        ImGui::InputText("Port",g.port,8);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputText("Name",g.name,64);
    }

    ImGui::Spacing();
    if(!ok){
        if(ImGui::Button("Connect",ImVec2(W-12,28)))DoConnect();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.28f,0.28f,0.36f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.35f,0.35f,0.45f,1.f));
        if(ImGui::Button("Disconnect",ImVec2(W-12,24)))DoDisconnect();
        ImGui::PopStyleColor(2);
    }

    if(g.syncing){
        ImGui::Spacing();
        ImGui::ProgressBar((float)g.progressCur/std::max(1,g.progressMax),
                           ImVec2(W-12,8),"");
        ImGui::TextDisabled("  %s  (%d/%d)",
                            g.syncStatus.c_str(),g.progressCur,g.progressMax);
    } else if(!g.statusText.empty()&&!ok){
        ImGui::TextDisabled("  %s",g.statusText.c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    float half=(W-6)*0.5f;

    ImGui::BeginChild("##players",ImVec2(half,130),true);
    SectionHeader("Players Online");
    if(g.players.empty()){
        ImGui::TextDisabled("  No players");
    } else {
        for(auto& p:g.players){

            ImDrawList* pdl=ImGui::GetWindowDrawList();
            ImVec2 pp=ImGui::GetCursorScreenPos();
            ImU32 pc=IM_COL32((int)(p.color.x*255),(int)(p.color.y*255),
                               (int)(p.color.z*255),255);
            pdl->AddCircleFilled(ImVec2(pp.x+6,pp.y+8),5.f,pc);
            ImGui::Dummy(ImVec2(14,0));ImGui::SameLine();
            ImGui::Text("%s",p.name.c_str());
            if(p.ping>0){
                ImGui::SameLine();
                ImGui::TextDisabled("%dms",p.ping);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d builds)",p.builds);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0,6);

    ImGui::BeginChild("##recentbuilds",ImVec2(half,130),true);
    SectionHeader("Recent Builds");
    int bn=(int)g.builds.size();
    int bstart=bn>5?bn-5:0;
    if(bn==0){
        ImGui::TextDisabled("  No builds yet");
    }
    for(int i=bstart;i<bn;i++){
        auto& b=g.builds[i];
        ImGui::TextColored(ImVec4(0.75f,0.25f,0.15f,1.f),
                           "%s",b.player.c_str());
        ImGui::SameLine();
        ImGui::Text("%s",b.type.c_str());
    }
    ImGui::EndChild();

    ImGui::Spacing();

    float chatH=ImGui::GetContentRegionAvail().y-34.f;
    if(chatH<60)chatH=60;
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.08f,0.08f,0.11f,1.f));
    ImGui::BeginChild("##chat",ImVec2(W,chatH),true);
    bool scrollToBot=(ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-4);
    for(auto& m:g.chat){
        ImGui::TextColored(ChatColor(m.type),"%s",m.text.c_str());
    }
    if(scrollToBot)ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(W-56);
    bool enter=ImGui::InputText("##ci",g.chatInput,256,
                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if((ImGui::Button("Send",ImVec2(48,0))||enter)&&g.connected)
        DoSendChat();
}

static void TabImport()
{
    float W=ImGui::GetContentRegionAvail().x;
    float half=(W-6)*0.5f;

    ImGui::BeginChild("##catpick",ImVec2(half,220),true);
    SectionHeader("Category");
    for(int i=0;i<g_catCount;i++){
        bool sel=(g.selectedCat==i);
        if(ImGui::Selectable(g_cats[i].name,sel,0,ImVec2(0,20))){
            g.selectedCat=i;
            FilterCat(i);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0,6);

    ImGui::BeginChild("##respick",ImVec2(half,220),true);
    SectionHeader("Resource");
    ImGui::SetNextItemWidth(half-20);
    if(ImGui::InputText("##rf",g.resFilter,64)){
        FilterCat(g.selectedCat);
    }
    ImGui::Separator();
    for(int i=0;i<(int)g.catResources.size();i++){
        bool sel=(g.selectedRes==i);
        if(ImGui::Selectable(g.catResources[i].c_str(),sel,0,ImVec2(0,20)))
            g.selectedRes=i;
    }
    if(g.catResources.empty())
        ImGui::TextDisabled("  Nothing found");
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.10f,0.10f,0.14f,1.f));
    ImGui::BeginChild("##reqform",ImVec2(W,90),true);
    SectionHeader("Send Request");

    std::string rname = g.selectedRes>=0&&g.selectedRes<(int)g.catResources.size()
                        ? g.catResources[g.selectedRes] : "(select resource)";
    ImGui::Text("Resource: ");ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f,0.65f,0.20f,1.f),"%s",rname.c_str());

    ImGui::SetNextItemWidth(110);
    ImGui::InputText("Amount",g.amountBuf,16,ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(0,16);
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("Price",g.priceBuf,16,ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(0,8);
    ImGui::TextDisabled("(0 = free)");

    bool canSend=g.connected&&g.selectedRes>=0;
    if(!canSend)ImGui::BeginDisabled();
    ImGui::SameLine(0,16);
    if(ImGui::Button("Send Request",ImVec2(120,22)))DoSendRequest();
    if(!canSend)ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();

    ImGui::BeginChild("##deals",ImVec2(W,0),true);
    SectionHeader("Resource Deals");

    if(g.deals.empty()){
        ImGui::TextDisabled("  No deals yet");
    } else {

        ImGui::Columns(5,"dealcols",true);
        ImGui::SetColumnWidth(0,80);
        ImGui::SetColumnWidth(1,80);
        ImGui::SetColumnWidth(2,120);
        ImGui::SetColumnWidth(3,60);
        ImGui::SetColumnWidth(4,70);
        ImGui::TextDisabled("From");ImGui::NextColumn();
        ImGui::TextDisabled("To");ImGui::NextColumn();
        ImGui::TextDisabled("Resource");ImGui::NextColumn();
        ImGui::TextDisabled("Amount");ImGui::NextColumn();
        ImGui::TextDisabled("Status");ImGui::NextColumn();
        ImGui::Separator();

        for(int i=(int)g.deals.size()-1;i>=0;i--){
            auto& d=g.deals[i];
            ImGui::Text("%s",d.from.c_str());ImGui::NextColumn();
            ImGui::Text("%s",d.to.empty()?"all":d.to.c_str());ImGui::NextColumn();
            ImGui::Text("%s",d.resource.c_str());ImGui::NextColumn();
            ImGui::Text("%d",d.amount);ImGui::NextColumn();
            if(d.pending){
                ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.9f,0.7f,0.1f,1.f));
                ImGui::Text("Pending");
                ImGui::PopStyleColor();

                if(d.to==g.name||d.to.empty()){
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.15f,0.55f,0.20f,1.f));
                    char abtn[16]; snprintf(abtn,16,"A##%d",i);
                    if(ImGui::SmallButton(abtn)){
                        d.pending=false;d.accepted=true;
                        char msg[128];
                        snprintf(msg,128,"[TRADE] Accepted: %d x %s from %s",
                                 d.amount,d.resource.c_str(),d.from.c_str());
                        AddChat(msg,CHAT_TRADE);
                        AddNotif("Deal accepted!",ImVec4(0.3f,0.9f,0.3f,1.f));
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    char dbtn[16]; snprintf(dbtn,16,"D##%d",i);
                    if(ImGui::SmallButton(dbtn)){
                        d.pending=false;d.accepted=false;
                    }
                }
            } else {
                if(d.accepted){
                    ImGui::TextColored(ImVec4(0.3f,0.85f,0.35f,1.f),"Accepted");
                } else {
                    ImGui::TextDisabled("Declined");
                }
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

    ImGui::BeginChild("##bstats",ImVec2(W,44),true);
    ImGui::Columns(3,"bstatscols",false);
    ImGui::TextDisabled("Total placed:");ImGui::SameLine();
    ImGui::Text("%d",(int)g.builds.size());
    ImGui::NextColumn();

    std::map<std::string,int> cnts;
    for(auto& b:g.builds)cnts[b.player]++;
    std::string topPlayer; int topCnt=0;
    for(auto& kv:cnts)if(kv.second>topCnt){topCnt=kv.second;topPlayer=kv.first;}
    if(!topPlayer.empty()){
        ImGui::TextDisabled("Most active:");ImGui::SameLine();
        ImGui::Text("%s (%d)",topPlayer.c_str(),topCnt);
    }
    ImGui::NextColumn();
    ImGui::TextDisabled("Filter:");ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##bf",g.buildFilter,64);
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::BeginChild("##buildlog",ImVec2(W,0),true);
    SectionHeader("Build History");

    if(g.builds.empty()){
        ImGui::TextDisabled("  No buildings placed yet in this session");
    } else {
        std::string flt=g.buildFilter;
        std::transform(flt.begin(),flt.end(),flt.begin(),::tolower);

        ImGui::Columns(5,"blogcols",true);
        ImGui::SetColumnWidth(0,90);
        ImGui::SetColumnWidth(1,160);
        ImGui::SetColumnWidth(2,80);
        ImGui::SetColumnWidth(3,80);
        ImGui::SetColumnWidth(4,80);
        ImGui::TextDisabled("Player");ImGui::NextColumn();
        ImGui::TextDisabled("Building");ImGui::NextColumn();
        ImGui::TextDisabled("X");ImGui::NextColumn();
        ImGui::TextDisabled("Z");ImGui::NextColumn();
        ImGui::TextDisabled("When");ImGui::NextColumn();
        ImGui::Separator();

        for(int i=(int)g.builds.size()-1;i>=0;i--){
            auto& b=g.builds[i];
            if(!flt.empty()){
                std::string lo=b.type+b.player;
                std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
                if(lo.find(flt)==std::string::npos)continue;
            }

            ImVec4 pc=ImVec4(0.9f,0.4f,0.2f,1.f);
            for(int pi=0;pi<(int)g.players.size();pi++)
                if(g.players[pi].name==b.player){pc=g.players[pi].color;break;}
            ImGui::TextColored(pc,"%s",b.player.c_str());ImGui::NextColumn();
            ImGui::Text("%s",b.type.c_str());ImGui::NextColumn();
            ImGui::Text("%.0f",b.x);ImGui::NextColumn();
            ImGui::Text("%.0f",b.z);ImGui::NextColumn();
            ImGui::TextDisabled("%s",FormatTime(b.timestamp).c_str());
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();
}

static void TabMap()
{
    float W=ImGui::GetContentRegionAvail().x;
    float H=ImGui::GetContentRegionAvail().y;

    ImGui::BeginChild("##mapctrl",ImVec2(W,32),false);
    ImGui::Text("Zoom:");ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("##zoom",&g.mapZoom,0.5f,4.f,"%.1fx");
    ImGui::SameLine(0,16);
    if(ImGui::Button("Reset View",ImVec2(90,22))){
        g.mapZoom=1.f;g.mapOffX=0;g.mapOffZ=0;
    }
    ImGui::SameLine(0,16);
    ImGui::TextDisabled("Buildings: %d",(int)g.builds.size());
    ImGui::EndChild();

    ImVec2 cpos=ImGui::GetCursorScreenPos();
    ImVec2 csz=ImVec2(W,H-36);
    ImDrawList* dl=ImGui::GetWindowDrawList();

    dl->AddRectFilled(cpos,ImVec2(cpos.x+csz.x,cpos.y+csz.y),
                      IM_COL32(14,22,14,255));
    dl->AddRect(cpos,ImVec2(cpos.x+csz.x,cpos.y+csz.y),
                IM_COL32(45,70,45,200));

    int gridN=8;
    for(int i=1;i<gridN;i++){
        float gx=cpos.x+csz.x*i/gridN;
        float gy=cpos.y+csz.y*i/gridN;
        dl->AddLine(ImVec2(gx,cpos.y),ImVec2(gx,cpos.y+csz.y),IM_COL32(30,45,30,180));
        dl->AddLine(ImVec2(cpos.x,gy),ImVec2(cpos.x+csz.x,gy),IM_COL32(30,45,30,180));
    }

    if(!g.builds.empty()){
        float minX=g.builds[0].x,maxX=g.builds[0].x;
        float minZ=g.builds[0].z,maxZ=g.builds[0].z;
        for(auto& b:g.builds){
            minX=std::min(minX,b.x);maxX=std::max(maxX,b.x);
            minZ=std::min(minZ,b.z);maxZ=std::max(maxZ,b.z);
        }
        float rX=maxX-minX; if(rX<500)rX=500;
        float rZ=maxZ-minZ; if(rZ<500)rZ=500;
        float pad=24.f;
        float scaleX=(csz.x-pad*2)*g.mapZoom/rX;
        float scaleZ=(csz.y-pad*2)*g.mapZoom/rZ;

        ImU32 pColors[4]={
            IM_COL32(230,75,50,220),
            IM_COL32(50,140,220,220),
            IM_COL32(50,200,80,220),
            IM_COL32(220,180,50,220)
        };

        for(int pi=0;pi<(int)g.players.size();pi++){
            auto& p=g.players[pi];
            if(!p.hasTerr)continue;
            float rx1=cpos.x+pad+(p.territory_x1-minX)*scaleX+g.mapOffX;
            float rz1=cpos.y+pad+(p.territory_z1-minZ)*scaleZ+g.mapOffZ;
            float rx2=cpos.x+pad+(p.territory_x2-minX)*scaleX+g.mapOffX;
            float rz2=cpos.y+pad+(p.territory_z2-minZ)*scaleZ+g.mapOffZ;
            ImU32 tc=pColors[pi%4];
            dl->AddRect(ImVec2(rx1,rz1),ImVec2(rx2,rz2),tc,0,0,1.5f);
            ImU32 tf=IM_COL32(
                (tc>>IM_COL32_R_SHIFT)&0xff,
                (tc>>IM_COL32_G_SHIFT)&0xff,
                (tc>>IM_COL32_B_SHIFT)&0xff,30);
            dl->AddRectFilled(ImVec2(rx1,rz1),ImVec2(rx2,rz2),tf);
        }

        for(auto& b:g.builds){
            float px=cpos.x+pad+(b.x-minX)*scaleX+g.mapOffX;
            float pz=cpos.y+pad+(b.z-minZ)*scaleZ+g.mapOffZ;
            if(px<cpos.x||px>cpos.x+csz.x||pz<cpos.y||pz>cpos.y+csz.y)continue;
            int ci=0;
            for(int pi=0;pi<(int)g.players.size();pi++)
                if(g.players[pi].name==b.player){ci=pi%4;break;}
            dl->AddCircleFilled(ImVec2(px,pz),4.f,pColors[ci]);
            dl->AddCircle(ImVec2(px,pz),4.f,IM_COL32(255,255,255,80));
        }

        float lx=cpos.x+8, ly=cpos.y+csz.y-20;
        for(int i=0;i<(int)g.players.size()&&i<4;i++){
            dl->AddCircleFilled(ImVec2(lx+5,ly+6),5,pColors[i]);
            lx+=12;
        }
    } else {

        const char* msg="No build data yet";
        ImVec2 ts=ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(cpos.x+(csz.x-ts.x)*0.5f,cpos.y+(csz.y-ts.y)*0.5f),
                    IM_COL32(60,90,60,255),msg);
    }

    ImGui::InvisibleButton("##mapbtn",csz);
    if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(0)){
        ImVec2 delta=ImGui::GetIO().MouseDelta;
        g.mapOffX+=delta.x;
        g.mapOffZ+=delta.y;
    }

    if(ImGui::IsItemHovered()){
        float wheel=ImGui::GetIO().MouseWheel;
        if(wheel!=0){g.mapZoom+=wheel*0.15f;g.mapZoom=std::max(0.3f,std::min(8.f,g.mapZoom));}
    }
}

static void TabStats()
{
    float W=ImGui::GetContentRegionAvail().x;
    float half=(W-6)*0.5f;

    ImGui::BeginChild("##sessionstats",ImVec2(half,150),true);
    SectionHeader("Session");
    ImGui::Text("Status:    ");ImGui::SameLine();
    ImGui::TextColored(g.connected?ImVec4(0.3f,0.9f,0.3f,1.f):ImVec4(0.8f,0.3f,0.3f,1.f),
                       g.connected?"Online":"Offline");
    ImGui::Text("Duration:  ");ImGui::SameLine();
    std::string dur=SessionDuration();
    ImGui::Text("%s",dur.empty()?"--":dur.c_str());
    ImGui::Text("Ping:      ");ImGui::SameLine();
    ImGui::Text("%s",g.connected?std::to_string(g.ping)+"ms":"--");
    ImGui::Text("Mode:      ");ImGui::SameLine();
    ImGui::Text("%s",g.isCoopMode?"Cooperative":"Territories");
    ImGui::EndChild();

    ImGui::SameLine(0,6);

    ImGui::BeginChild("##actstats",ImVec2(half,150),true);
    SectionHeader("Activity");
    ImGui::Text("Buildings placed:  ");ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.builds.size());
    ImGui::Text("Chat messages:     ");ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",g.totalMessages);
    ImGui::Text("Resource requests: ");ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.deals.size());
    ImGui::Text("Players online:    ");ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f,0.5f,0.2f,1.f),"%d",(int)g.players.size());
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::BeginChild("##playerstats",ImVec2(W,0),true);
    SectionHeader("Player Stats");
    if(g.players.empty()){
        ImGui::TextDisabled("  No players connected");
    } else {
        ImGui::Columns(4,"pscols",true);
        ImGui::SetColumnWidth(0,120);
        ImGui::SetColumnWidth(1,70);
        ImGui::SetColumnWidth(2,70);
        ImGui::TextDisabled("Player");ImGui::NextColumn();
        ImGui::TextDisabled("Ping");ImGui::NextColumn();
        ImGui::TextDisabled("Builds");ImGui::NextColumn();
        ImGui::TextDisabled("Status");ImGui::NextColumn();
        ImGui::Separator();
        for(auto& p:g.players){
            ImGui::TextColored(p.color,"%s",p.name.c_str());ImGui::NextColumn();
            ImGui::Text("%dms",p.ping);ImGui::NextColumn();
            ImGui::Text("%d",p.builds);ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1.f),"Online");
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();
}

static bool CreateDX11(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd={};
    sd.BufferCount=2;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hwnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,
        NULL,0,NULL,0,D3D11_SDK_VERSION,&sd,&g_chain,&g_dev,&fl,&g_ctx)))
        return false;
    ID3D11Texture2D* bb=NULL;
    g_chain->GetBuffer(0,IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb,NULL,&g_rtv);
    bb->Release();
    return true;
}
static void CleanDX11()
{
    if(g_rtv){g_rtv->Release();g_rtv=NULL;}
    if(g_chain){g_chain->Release();g_chain=NULL;}
    if(g_ctx){g_ctx->Release();g_ctx=NULL;}
    if(g_dev){g_dev->Release();g_dev=NULL;}
}
static void ResizeDX11(UINT w,UINT h)
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
    case WM_SIZE:
        if(g_dev&&wp!=SIZE_MINIMIZED)ResizeDX11(LOWORD(lp),HIWORD(lp));
        return 0;
    case WM_TIMER:
        if(wp==1&&g.connected){
            g.lastPingTime=GetTickCount64();
            NetSendMsg(g.sock,MSG_PING,nullptr,0);
        }
        return 0;
    case WM_SYSCOMMAND:
        if((wp&0xfff0)==SC_KEYMENU)return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int)
{
    InitializeCriticalSection(&g.sendLock);

    WNDCLASSEXA wc={sizeof(wc)};
    wc.style=CS_CLASSDC; wc.lpfnWndProc=WndProc;
    wc.hInstance=hInst; wc.lpszClassName="WRSRMp";
    wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    RegisterClassExA(&wc);

    g_hwnd=CreateWindowA("WRSRMp",
        "WRSR Multiplayer  v0.4.1",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,340,680,
        NULL,NULL,hInst,NULL);

    if(!CreateDX11(g_hwnd)){CleanDX11();return 1;}

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename=NULL;

    io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev,g_ctx);

    ApplyTheme();
    LoadResources();
    FilterCat(0);

    ShowWindow(g_hwnd,SW_SHOW);
    UpdateWindow(g_hwnd);

    const float CC[4]={0.08f,0.08f,0.10f,1.f};

    MSG msg={};
    while(g_running){
        while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){
            TranslateMessage(&msg);DispatchMessageA(&msg);
            if(msg.message==WM_QUIT)g_running=false;
        }
        if(!g_running)break;

        PollSharedMemory();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io2=ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(io2.DisplaySize);
        ImGui::Begin("##root",NULL,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);

        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.88f,0.28f,0.18f,1.f));
        ImGui::Text("  WRSR Multiplayer");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("v0.4.1  |  Kobel-WRSR");
        ImGui::SameLine(io2.DisplaySize.x-90);
        if(g.connected){
            ImGui::TextColored(ImVec4(0.3f,0.85f,0.3f,1.f),"ONLINE");
        } else {
            ImGui::TextDisabled("OFFLINE");
        }
        ImGui::Separator();

        if(ImGui::BeginTabBar("##maintabs",ImGuiTabBarFlags_None)){
            if(ImGui::BeginTabItem("Main"))      {TabMain();     ImGui::EndTabItem();}
            if(ImGui::BeginTabItem("Import"))    {TabImport();   ImGui::EndTabItem();}
            if(ImGui::BeginTabItem("Buildings")) {TabBuildings();ImGui::EndTabItem();}
            if(ImGui::BeginTabItem("Map"))       {TabMap();      ImGui::EndTabItem();}
            if(ImGui::BeginTabItem("Stats"))     {TabStats();    ImGui::EndTabItem();}
            ImGui::EndTabBar();
        }

        double now=ImGui::GetTime();
        float ny=io2.DisplaySize.y-16;
        for(int i=(int)g.notifications.size()-1;i>=0;i--){
            auto& n=g.notifications[i];
            double elapsed=now-n.timestamp;
            if(elapsed>3.5){g.notifications.erase(g.notifications.begin()+i);continue;}
            float alpha=(float)(elapsed>2.5?(3.5-elapsed):1.0);
            ny-=28;
            ImGui::SetNextWindowBgAlpha(0.82f*alpha);
            ImGui::SetNextWindowPos(ImVec2(io2.DisplaySize.x*0.5f,ny),
                ImGuiCond_Always,ImVec2(0.5f,0.f));
            char wid[16]; snprintf(wid,16,"##n%d",i);
            ImGui::Begin(wid,NULL,
                ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
                ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
                ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoMove);
            ImGui::TextColored(ImVec4(n.color.x,n.color.y,n.color.z,alpha),
                               "  %s  ",n.text.c_str());
            ImGui::End();
        }

        ImGui::End();
        ImGui::Render();

        g_ctx->OMSetRenderTargets(1,&g_rtv,NULL);
        g_ctx->ClearRenderTargetView(g_rtv,CC);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_chain->Present(1,0);
    }

    DoDisconnect();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanDX11();
    DestroyWindow(g_hwnd);
    UnregisterClassA("WRSRMp",hInst);
    g_shm.Destroy();
    DeleteCriticalSection(&g.sendLock);
    return 0;
}