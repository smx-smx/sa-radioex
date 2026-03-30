#include "RadioEx.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <windef.h>
#include <minwinbase.h>
#include <wininet.h>
#include <process.h>
#include <MinHook.h>
#include <stdexcept>
#include <bits/shared_ptr.h>

#define NOBASSOVERLOADS
#include "bass.h"

static typeof(&BASS_SetConfig) pBASS_SetConfig;
static typeof(&BASS_GetVersion) pBASS_GetVersion;
static typeof(&BASS_Init) pBASS_Init;
static typeof(&BASS_Free) pBASS_Free;
static typeof(&BASS_StreamCreateFile) pBASS_StreamCreateFile;
static typeof(&BASS_StreamCreateURL) pBASS_StreamCreateURL;
static typeof(&BASS_StreamFree) pBASS_StreamFree;
static typeof(&BASS_ChannelPlay) pBASS_ChannelPlay;
static typeof(&BASS_ChannelStop) pBASS_ChannelStop;
static typeof(&BASS_ChannelSetAttribute) pBASS_ChannelSetAttribute;
static typeof(&BASS_StreamGetFilePosition) pBASS_StreamGetFilePosition;
static typeof(&BASS_ErrorGetCode) pBASS_ErrorGetCode;
static typeof(&BASS_PluginLoad) pBASS_PluginLoad;
static typeof(&BASS_PluginFree) pBASS_PluginFree;
static typeof(&BASS_ChannelIsActive) pBASS_ChannelIsActive;

static HMODULE hBass = nullptr;

static std::array<TRadioStation, 12> Radio;

static char RadioNStr[256];
static int LastChlIndex = -1;
static HANDLE ChlThread = nullptr;
static HWND GTAWnd = nullptr;
static HANDLE PoolThread = nullptr;
static bool RadioStopped = true;
static bool TxPatch = false;
static bool EnableBassInit = true;
static bool BassInited = false;
static CRITICAL_SECTION MainThreadCs;
static bool PoolThreadCancellation = false;
static bool StreamThreadCancellation = false;

static TResStream LoadingRs, NoRadioRs, NoIConnRs;

static HPLUGIN AACPlugin = 0;
static HPLUGIN OPSPlugin = 0;

static HINSTANCE hThisModule;
static HSTREAM ActiveStream = 0;

FILE *hDebugLog = nullptr;

static bool RadioErrorState = false;

#define DLOG(fmt, ...) ({if(hDebugLog) { LogPre(); fprintf(hDebugLog, fmt "\n", ##__VA_ARGS__); }})

static void LogPre() {
    time_t now = time(nullptr);
    tm *local = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", local);
    fprintf(hDebugLog, "[%s] ", time_str);
}

template<typename T>
void TGetProcAddress(HMODULE hModule, LPCSTR lpProcName, T& pfnRef) {
    pfnRef = reinterpret_cast<T>(GetProcAddress(hModule, lpProcName));
}

template<typename TTarget>
MH_STATUS TMH_CreateHook(TTarget target, TTarget detour, TTarget &origRef) {
    return MH_CreateHook(
        reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(detour),
        reinterpret_cast<LPVOID*>(&origRef)
    );
}

template<typename TTarget>
MH_STATUS TMH_EnableHook(TTarget target) {
    return MH_EnableHook(reinterpret_cast<LPVOID>(target));
}

template<typename TTarget>
MH_STATUS TMH_DisableHook(TTarget target) {
    return MH_DisableHook(reinterpret_cast<LPVOID>(target));
}

template<typename TTarget>
MH_STATUS TMH_RemoveHook(TTarget target) {
    return MH_RemoveHook(reinterpret_cast<LPVOID>(target));
}

static int hookRefCount = 0;

template<typename TTarget>
struct THook {
private:
    TTarget m_target;
    TTarget m_detour;
    bool m_created;
public:
    typeof(TTarget) origFunc = nullptr;

    THook(TTarget target, TTarget detour)
        : m_target(target), m_detour(detour), m_created(false)
    {
    }

    void enable() {
        if (hookRefCount++ == 0) {
            if (MH_Initialize() != MH_OK) {
                DLOG("MH_Initialize() failed");
                throw std::runtime_error("MH_Initialize() failed");
            }
        }

        if (!m_created) {
            if (TMH_CreateHook(m_target, m_detour, origFunc) != MH_OK) {
                DLOG("TMH_CreateHook failed");
                throw std::runtime_error("Failed to create hook");
            }
            m_created = true;
        }
        if (TMH_EnableHook(m_target) != MH_OK) {
            DLOG("TMH_EnableHook failed");
            throw std::runtime_error("Failed to enable hook");
        }
    }

    void disable() {
        TMH_DisableHook(m_target);
    }

    ~THook() {
        if (m_created) {
            TMH_RemoveHook(m_target);
        }
        if (--hookRefCount == 0) {
            MH_Uninitialize();
        }
    }
};

static char __stdcall *my_GetRadioStationName(char index);
static int __thiscall my_StartTrackPlayback(PVOID pThis);

static THook hk_GetRadioStationName(GTEXT_GetRadioStationName, my_GetRadioStationName);
static THook hk_StartTrackPlayback(GTEXT_StartTrackPlayback, my_StartTrackPlayback);

static bool IsRadioOn(const uint8_t *state) {
    if (*(DWORD *)(&state[26*4]) != 7) return true;
    if (*state) return true;
    if (*(DWORD *)(&state[27*4])) return true;
    if (*(DWORD *)(&state[28*4])) return true;
    return false;
}

char GetCurrentRadioStationID(const uint8_t *state) {
    char sta = state[233];
    if (sta == -1) {
        return 13;
    }
    return sta;
}

void SetRadioState(PBYTE state, BYTE newState) {
    state[104] = newState;
}

int __thiscall my_StartTrackPlayback(PVOID pThis) {
    do {
        if (!IsRadioOn(&GDATA_RadioState[0])) {
            break;
        }

        char radioIndex = GetCurrentRadioStationID(&GDATA_RadioState[0]);
        if (radioIndex < 0 || radioIndex >= Radio.size()) {
            // invalid state, allow playback
            break;
        }

        if (!Radio[radioIndex].Enable) {
            // allow playback
            break;
        }

        if (RadioErrorState) {
            // cannot load radio, allow playback
            break;
        }

        // prevent playback
        return 0;
    } while (false);

    return hk_StartTrackPlayback.origFunc(pThis);
}

HWND GetGTAWindow() {
    return FindWindowA("Grand theft auto San Andreas", "GTA: San Andreas");
}

TResStream LoadRescData(const char* Name) {
    TResStream res;
    res.Handle = FindResourceA(hThisModule, Name, RT_RCDATA);
    res.Location = LockResource(LoadResource(hThisModule, res.Handle));
    res.Size = SizeofResource(hThisModule, res.Handle);
    return res;
}

BOOL IsConnected() {
    DWORD flags;
    return InternetGetConnectedState(&flags, 0);
}


static void SetRadioErrorState() {
    RadioErrorState = true;
    // resume radio playback
    SetRadioState(&GDATA_RadioState[0], 1);
}

static void WaitForStreamEnd(const HSTREAM stream) {
    if (stream == 0) return;
    while (!StreamThreadCancellation) {
        if (pBASS_ChannelIsActive(stream) != BASS_ACTIVE_PLAYING) break;
        Sleep(10);
    }
}

static void StreamChannel_WaitBuffer(const HSTREAM stream, const DWORD thresholdPct = 75) {
    if (stream == 0) return;

    QWORD total = pBASS_StreamGetFilePosition(stream, BASS_FILEPOS_END);
    if (static_cast<long long>(total) == -1) return;

    while (!StreamThreadCancellation) {
        QWORD buffered = pBASS_StreamGetFilePosition(stream, BASS_FILEPOS_BUFFER);
        if (static_cast<long long>(buffered) == -1) break;
        if (static_cast<DWORD>(buffered * 100 / total) > thresholdPct) break;

        if (GetForegroundWindow() == GTAWnd) {
            pBASS_ChannelSetAttribute(stream, BASS_ATTRIB_VOL, GDATA_RadioVolume);
        } else {
            // Window lost focus, mute it!
            pBASS_ChannelSetAttribute(stream, BASS_ATTRIB_VOL, 0);
        }

        Sleep(10);
    }
}

unsigned int __stdcall StreamChannelThread(void* p) {
    TRadioStation *sta = reinterpret_cast<TRadioStation*>(p);

    HSTREAM loadingSound = pBASS_StreamCreateFile(TRUE, LoadingRs.Location, 0, LoadingRs.Size, BASS_SAMPLE_LOOP | BASS_STREAM_AUTOFREE);
    pBASS_ChannelPlay(loadingSound, true);
    pBASS_ChannelSetAttribute(loadingSound, BASS_ATTRIB_VOL, GDATA_RadioVolume);

    if (StreamThreadCancellation) {
        pBASS_ChannelStop(loadingSound);
        pBASS_StreamFree(loadingSound);
        return EXIT_FAILURE;
    }

    bool playbackError = false;
    DLOG("Starting: %s", sta->Location);
    HSTREAM stream = pBASS_StreamCreateURL(sta->Location, 0, BASS_STREAM_BLOCK | BASS_STREAM_AUTOFREE, nullptr, nullptr);
    if (stream == 0) {
        stream = pBASS_StreamCreateFile(TRUE, NoRadioRs.Location, 0, NoRadioRs.Size, BASS_STREAM_AUTOFREE);
        playbackError = true;
    }

    StreamChannel_WaitBuffer(stream);
    pBASS_ChannelStop(loadingSound);
    pBASS_StreamFree(loadingSound);

    if (StreamThreadCancellation) {
        pBASS_ChannelStop(stream);
        pBASS_StreamFree(stream);
        return EXIT_FAILURE;
    }

    pBASS_ChannelPlay(stream, FALSE);
    pBASS_ChannelSetAttribute(stream, BASS_ATTRIB_VOL, GDATA_RadioVolume);
    RadioStopped = false;

    if (playbackError) {
        WaitForStreamEnd(stream);
        SetRadioErrorState();
    }

    ChlThread = nullptr;
    // stream plays on; freed when StopRadioFunc kills it via ActiveStream
    ActiveStream = stream;
    return EXIT_SUCCESS;
}

void StartChannel(int radioIndex) {
    if (IsConnected()) {
        if (ChlThread != nullptr) {
            StreamThreadCancellation = true;
            WaitForSingleObject(ChlThread, INFINITE);
            CloseHandle(ChlThread);
        }
        StreamThreadCancellation = false;
        ChlThread = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, StreamChannelThread,
                reinterpret_cast<void *>(&Radio[radioIndex]), 0, nullptr));
    } else {
        HSTREAM stream = pBASS_StreamCreateFile(TRUE, NoIConnRs.Location, 0, NoIConnRs.Size, BASS_STREAM_AUTOFREE);
        pBASS_ChannelPlay(stream, FALSE);
        pBASS_ChannelSetAttribute(stream, BASS_ATTRIB_VOL, GDATA_RadioVolume);
        RadioStopped = false;
        WaitForStreamEnd(stream);
        SetRadioErrorState();
    }
}

void StopRadioFunc() {
    if (RadioStopped) {
        RadioErrorState = false;
        return;
    }

    StreamThreadCancellation = true;

    if (ChlThread != nullptr) {
        WaitForSingleObject(ChlThread, INFINITE);
        CloseHandle(ChlThread);
        ChlThread = nullptr;
    }

    if (ActiveStream != 0) {
        pBASS_ChannelStop(ActiveStream);
        pBASS_StreamFree(ActiveStream);
        ActiveStream = 0;
    }

    RadioStopped = true;
    LastChlIndex = -1;
    RadioErrorState = false;
}

unsigned int __stdcall PoolProc(void* p) {
    DLOG("Watcher Thread starting");
    while (!PoolThreadCancellation) {
        if (!IsWindow(GTAWnd)) {
            GTAWnd = GetGTAWindow();
            if (IsWindow(GTAWnd)) {
                EnterCriticalSection(&MainThreadCs);
                if (EnableBassInit && pBASS_Init) {
                    if (pBASS_Init(-1, 44100, 0, GTAWnd, nullptr)) {
                        BassInited = TRUE;
                    } else if (pBASS_ErrorGetCode() != BASS_ERROR_ALREADY) {
                        ShowWindow(GTAWnd, SW_HIDE);
                        MessageBoxA(GTAWnd, "Can't initialize device", nullptr, MB_ICONERROR);
                        ShowWindow(GTAWnd, SW_SHOW);
                    }
                }
                LeaveCriticalSection(&MainThreadCs);
            }
        }

        do {
            if (!IsRadioOn(&GDATA_RadioState[0])) {
                // radio was turned off, stop
                StopRadioFunc();
                break;
            }

            char radioIndex = GetCurrentRadioStationID(&GDATA_RadioState[0]);
            bool changingStation = radioIndex != LastChlIndex;
            if (changingStation) {
                // stop ongoing stream, if any
                StopRadioFunc();
            }

            if (radioIndex < 0 || radioIndex >= Radio.size()) {
                // invalid index, can't play
                break;
            }
            if (!Radio[radioIndex].Enable) {
                // disabled channel, won't play
                break;
            }

            if (changingStation) {
                if (pBASS_ChannelSetAttribute) {

                }
                // start new station and update index
                StartChannel(radioIndex);
                LastChlIndex = radioIndex;
            }

        } while (false);

        // Delay for 1/100 second
        Sleep(10);
    }

    PoolThread = nullptr;
    return 0;
}

static char* GetRadioName(char index) {
    strncpy(RadioNStr, Radio[index].Name, sizeof(RadioNStr) - 1);
    RadioNStr[sizeof(RadioNStr) - 1] = '\0';
    return RadioNStr;
}

char __stdcall *my_GetRadioStationName(char index){
    DLOG("radio %d", index);
    if(index < 0 || index >= Radio.size()){
        return hk_GetRadioStationName.origFunc(index);
    }
    return GetRadioName(index);
}

bool InitAsi() {
    InitializeCriticalSection(&MainThreadCs);
    char mainDir[MAX_PATH];
    GetModuleFileNameA(hThisModule, mainDir, MAX_PATH); // Use hModule
    if (char* lastSlash = strrchr(mainDir, '\\')) {
        *(lastSlash + 1) = '\0';
    }
    else mainDir[0] = '\0';

    char iniPath[MAX_PATH];

    // momentarily use iniPath for logFilePath
    sprintf(iniPath, "%sRadioEx.log", mainDir);
    hDebugLog = fopen(iniPath, "w+b");
    if(hDebugLog){
        setvbuf(hDebugLog, nullptr, _IONBF, 0);
    }
    DLOG("RadioEx starting...");

    sprintf(iniPath, "%sRadioEx.ini", mainDir);

    EnableBassInit = GetPrivateProfileIntA("Options", "EnableBassInit", 1, iniPath) ? TRUE : FALSE;
    char lang[10];
    GetPrivateProfileStringA("Options", "NoticeLang", "ID", lang, 10, iniPath);
    BOOL EnableTxPatch = GetPrivateProfileIntA("Options", "RadioNamePatch", 1, iniPath) ? TRUE : FALSE;
    BOOL AACPlug = GetPrivateProfileIntA("Plugins", "BASS_AAC", 0, iniPath) ? TRUE : FALSE;
    BOOL OPSPlug = GetPrivateProfileIntA("Plugins", "BASS_OPUS", 0, iniPath) ? TRUE : FALSE;

    for (int i = 0; i < Radio.size(); i++) {
        char key[50];
        sprintf(key, "Track%d_Enable", i);
        Radio[i].Enable = GetPrivateProfileIntA("RadioChannels", key, 0, iniPath) ? TRUE : FALSE;
        sprintf(key, "Track%d_Name", i);
        GetPrivateProfileStringA("RadioChannels", key, "", Radio[i].Name, 256, iniPath);
        sprintf(key, "Track%d_URL", i);
        GetPrivateProfileStringA("RadioChannels", key, "", Radio[i].Location, 1024, iniPath);

        DLOG("Radio[%d]: Enable:%s Name='%s', URL='%s'",
            i,
            Radio[i].Enable ? "TRUE" : "FALSE",
            Radio[i].Name,
            Radio[i].Location);
    }

    // Load notice sound
    LoadingRs = LoadRescData("LOADING");
    char resName[50];
    sprintf(resName, "NORADIO_%s", lang);
    NoRadioRs = LoadRescData(resName);
    sprintf(resName, "NOICONN_%s", lang);
    NoIConnRs = LoadRescData(resName);

    hBass = LoadLibraryA("bass.dll");
    if (hBass) {
        TGetProcAddress(hBass, "BASS_SetConfig", pBASS_SetConfig);
        TGetProcAddress(hBass, "BASS_GetVersion", pBASS_GetVersion);
        TGetProcAddress(hBass, "BASS_Init", pBASS_Init);
        TGetProcAddress(hBass, "BASS_Free", pBASS_Free);
        TGetProcAddress(hBass, "BASS_StreamCreateFile", pBASS_StreamCreateFile);
        TGetProcAddress(hBass, "BASS_StreamCreateURL", pBASS_StreamCreateURL);
        TGetProcAddress(hBass, "BASS_StreamFree", pBASS_StreamFree);
        TGetProcAddress(hBass, "BASS_ChannelPlay", pBASS_ChannelPlay);
        TGetProcAddress(hBass, "BASS_ChannelStop", pBASS_ChannelStop);
        TGetProcAddress(hBass, "BASS_ChannelSetAttribute", pBASS_ChannelSetAttribute);
        TGetProcAddress(hBass, "BASS_StreamGetFilePosition", pBASS_StreamGetFilePosition);
        TGetProcAddress(hBass, "BASS_ErrorGetCode", pBASS_ErrorGetCode);
        TGetProcAddress(hBass, "BASS_PluginLoad", pBASS_PluginLoad);
        TGetProcAddress(hBass, "BASS_PluginFree", pBASS_PluginFree);
        TGetProcAddress(hBass, "BASS_ChannelIsActive", pBASS_ChannelIsActive);
    }

    if (pBASS_GetVersion && (HIWORD(pBASS_GetVersion()) != BASSVERSION)) {
        DLOG("An incorrect version of BASS.DLL was loaded");
        return false;
    }

    if (pBASS_SetConfig) {
        pBASS_SetConfig(BASS_CONFIG_NET_PLAYLIST, 1);
        pBASS_SetConfig(BASS_CONFIG_NET_PREBUF, 0);
    }

    if (AACPlug && pBASS_PluginLoad) AACPlugin = pBASS_PluginLoad("bass_aac.dll", 0);
    if (OPSPlug && pBASS_PluginLoad) OPSPlugin = pBASS_PluginLoad("bassopus.dll", 0);

    PoolThreadCancellation = FALSE;
    DLOG("Starting watcher thread...");
    PoolThread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 0, PoolProc, nullptr, 0, nullptr));


    // Patch LoadString
    if (EnableTxPatch) {
        hk_GetRadioStationName.enable();
        TxPatch = TRUE;
    }

    hk_StartTrackPlayback.enable();

    return true;
}


void UnInitAsi() {
    StreamThreadCancellation = TRUE;
    PoolThreadCancellation = TRUE;


    if (PoolThread) {
        WaitForSingleObject(PoolThread, INFINITE);
        CloseHandle(PoolThread);
    }
    DeleteCriticalSection(&MainThreadCs);

    if (AACPlugin && pBASS_PluginFree) pBASS_PluginFree(AACPlugin);
    if (OPSPlugin && pBASS_PluginFree) pBASS_PluginFree(OPSPlugin);
    if (EnableBassInit && pBASS_Free) pBASS_Free();

    if (hDebugLog) {
        fclose(hDebugLog);
        hDebugLog = nullptr;
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    hThisModule = hinstDLL;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            InitAsi();
            break;
        case DLL_PROCESS_DETACH:
            UnInitAsi();
            break;
        default:;
    }
    return TRUE;
}
