#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <minwindef.h>
#include <stdio.h>
#include <stdint.h>

// BASS definitions (minimal)
typedef DWORD HSTREAM;
typedef DWORD HPLUGIN;
typedef uint64_t QWORD;
typedef float FLOAT;

// GTA Addresses (v1.0)
extern char* __stdcall GTEXT_GetRadioStationName(char);
extern char GDATA_RadioIndex;
extern BYTE GDATA_StopStatus1;
extern BYTE* GDATA_StopStatus2;
const off_t Stop2Ptrloc = 0x530;
extern float GDATA_RadioVolume;
extern bool __thiscall GTEXT_IsRadioOn(PVOID pThis);
extern int __thiscall GTEXT_StartTrackPlayback(PVOID pThis);
extern BYTE GDATA_RadioState[128];

enum class eRadioExState {
    S_STOPPED,
    S_TRACKING,
    S_PLAYING,
    S_ERROR
};

typedef struct {
    HRSRC Handle;
    void* Location;
    DWORD Size;
} TResStream;

typedef struct {
    BOOL Enable;
    char Name[256];
    char Location[1024];
} TRadioStation;

#define FakeFileSize 0xA00000 // 10MB
#define StreamPath "AUDIO\\STREAMS\\"

#ifdef __cplusplus
}
#endif