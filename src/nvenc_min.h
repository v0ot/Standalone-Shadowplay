// nvenc_min.h — minimum NVENC API declarations needed to drive nvEncodeAPI64.dll.
// Transcribed from NVIDIA's public Video Codec SDK headers (nvEncodeAPI.h)
// at NVENCAPI_VERSION 12.x. Kept ABI-compatible with the version shipped in
// recent NVIDIA display drivers.
//
// We declare only the structs/enums/fn-pointer-table fields actually used.
#pragma once
#include <windows.h>
#include <cstdint>

#define NVENCAPI __stdcall
#define NVENCAPI_MAJOR_VERSION 13
#define NVENCAPI_MINOR_VERSION 0
#define NVENCAPI_VERSION  ((NVENCAPI_MAJOR_VERSION) | ((NVENCAPI_MINOR_VERSION) << 24))
#define NVENCAPI_STRUCT_VERSION(ver) ((uint32_t)NVENCAPI_VERSION | ((ver) << 16) | (0x7 << 28))

typedef int NVENCSTATUS;
#define NV_ENC_SUCCESS 0

typedef struct _GUID2 { uint32_t Data1; uint16_t Data2,Data3; uint8_t Data4[8]; } GUID2;

// Codec / preset GUIDs (verbatim from NVIDIA's header).
static const GUID2 NV_ENC_CODEC_H264_GUID =
    {0x6BC82762,0x4E63,0x4ca4,{0xAA,0x85,0x1E,0x50,0xF3,0x21,0xF6,0xBF}};
static const GUID2 NV_ENC_PRESET_P4_GUID =
    {0x90A7B826,0x42F8,0x4bd4,{0x95,0xDC,0xB9,0xC5,0x71,0x37,0x9E,0xD3}};
static const GUID2 NV_ENC_H264_PROFILE_HIGH_GUID =
    {0xE7CBC309,0x4F7A,0x4b89,{0xAF,0x2A,0xD5,0x37,0xC9,0x2B,0xE3,0x10}};

enum { NV_ENC_DEVICE_TYPE_DIRECTX = 0 };
enum NV_ENC_BUFFER_FORMAT {
    NV_ENC_BUFFER_FORMAT_UNDEFINED = 0,
    NV_ENC_BUFFER_FORMAT_ARGB      = 0x01000000,
    NV_ENC_BUFFER_FORMAT_ABGR      = 0x02000000,
};
enum NV_ENC_PIC_STRUCT { NV_ENC_PIC_STRUCT_FRAME = 0x01 };
enum NV_ENC_INPUT_RESOURCE_TYPE { NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX = 0 };
enum NV_ENC_TUNING_INFO {
    NV_ENC_TUNING_INFO_HIGH_QUALITY  = 1,
    NV_ENC_TUNING_INFO_LOW_LATENCY   = 2,
    NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY = 3,
};
enum NV_ENC_PARAMS_RC_MODE {
    NV_ENC_PARAMS_RC_CONSTQP = 0x0,
    NV_ENC_PARAMS_RC_VBR     = 0x1,
    NV_ENC_PARAMS_RC_CBR     = 0x2,
};

typedef struct _NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    uint32_t version;
    uint32_t deviceType;
    void*    device;
    void*    reserved;
    uint32_t apiVersion;
    uint32_t reserved1[253];
    void*    reserved2[64];
} NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS;
#define NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER NVENCAPI_STRUCT_VERSION(1)

typedef struct _NV_ENC_QP { uint32_t qpInterP, qpInterB, qpIntra; } NV_ENC_QP;

typedef struct _NV_ENC_RC_PARAMS {
    uint32_t              version;
    NV_ENC_PARAMS_RC_MODE rateControlMode;
    NV_ENC_QP             constQP;
    uint32_t              averageBitRate;
    uint32_t              maxBitRate;
    uint32_t              vbvBufferSize;
    uint32_t              vbvInitialDelay;
    uint32_t              flags;
    uint32_t              padding[55];
} NV_ENC_RC_PARAMS;

typedef struct _NV_ENC_CONFIG_H264 {
    uint32_t flags;
    uint32_t level;
    uint32_t idrPeriod;
    uint32_t separateColourPlaneFlag;
    uint32_t disableDeblockingFilterIDC;
    uint32_t numTemporalLayers;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t adaptiveTransformMode;
    uint32_t fmoMode;
    uint32_t bdirectMode;
    uint32_t entropyCodingMode;
    uint32_t stereoMode;
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t maxNumRefFrames;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint8_t  reserved[2048];
} NV_ENC_CONFIG_H264;

typedef struct _NV_ENC_CODEC_CONFIG {
    NV_ENC_CONFIG_H264 h264Config;
    uint8_t            reserved[256];
} NV_ENC_CODEC_CONFIG;

typedef struct _NV_ENC_CONFIG {
    uint32_t            version;
    GUID2               profileGUID;
    uint32_t            gopLength;
    int32_t             frameIntervalP;
    uint32_t            monoChromeEncoding;
    uint32_t            frameFieldMode;
    uint32_t            mvPrecision;
    NV_ENC_RC_PARAMS    rcParams;
    NV_ENC_CODEC_CONFIG encodeCodecConfig;
    uint32_t            padding[278];
} NV_ENC_CONFIG;
#define NV_ENC_CONFIG_VER NVENCAPI_STRUCT_VERSION(7) | (1 << 31)

typedef struct _NV_ENC_INITIALIZE_PARAMS {
    uint32_t       version;
    GUID2          encodeGUID;
    GUID2          presetGUID;
    uint32_t       encodeWidth;
    uint32_t       encodeHeight;
    uint32_t       darWidth;
    uint32_t       darHeight;
    uint32_t       frameRateNum;
    uint32_t       frameRateDen;
    uint32_t       enableEncodeAsync;
    uint32_t       enablePTD;
    uint32_t       reportSliceOffsets;
    uint32_t       enableSubFrameWrite;
    uint32_t       enableExternalMEHints;
    uint32_t       enableMEOnlyMode;
    uint32_t       enableWeightedPrediction;
    uint32_t       enableOutputInVidmem;
    uint32_t       enableReconFrameOutput;
    uint32_t       enableOutputStats;
    uint32_t       reservedBitFields;
    uint32_t       privDataSize;
    void*          privData;
    NV_ENC_CONFIG* encodeConfig;
    uint32_t       maxEncodeWidth;
    uint32_t       maxEncodeHeight;
    void*          maxMEHintCountsPerBlock[2];
    NV_ENC_TUNING_INFO tuningInfo;
    int            bufferFormat;
    uint32_t       numStateBuffers;
    uint32_t       outputStatsLevel;
    uint32_t       splitEncodeMode;
    uint32_t       reserved[284];
    void*          reserved2[64];
} NV_ENC_INITIALIZE_PARAMS;
#define NV_ENC_INITIALIZE_PARAMS_VER NVENCAPI_STRUCT_VERSION(5) | (1 << 31)

typedef struct _NV_ENC_PRESET_CONFIG {
    uint32_t      version;
    NV_ENC_CONFIG presetCfg;
    uint32_t      reserved1[255];
    void*         reserved2[64];
} NV_ENC_PRESET_CONFIG;
#define NV_ENC_PRESET_CONFIG_VER NVENCAPI_STRUCT_VERSION(4) | (1 << 31)

typedef struct _NV_ENC_CREATE_BITSTREAM_BUFFER {
    uint32_t version;
    uint32_t size;             // deprecated, set 0
    uint32_t memoryHeap;
    uint32_t reserved;
    void*    bitstreamBuffer;  // OUT
    void*    bitstreamBufferPtr;
    uint32_t reserved1[58];
    void*    reserved2[64];
} NV_ENC_CREATE_BITSTREAM_BUFFER;
#define NV_ENC_CREATE_BITSTREAM_BUFFER_VER NVENCAPI_STRUCT_VERSION(1)

typedef struct _NV_ENC_REGISTER_RESOURCE {
    uint32_t version;
    int      resourceType;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t subResourceIndex;
    void*    resourceToRegister;
    void*    registeredResource;  // OUT
    NV_ENC_BUFFER_FORMAT bufferFormat;
    uint32_t bufferUsage;
    void*    pInputFencePoint;
    uint32_t chromaOffset[2];
    uint32_t reserved1[112];
    void*    reserved2[60];
} NV_ENC_REGISTER_RESOURCE;
#define NV_ENC_REGISTER_RESOURCE_VER NVENCAPI_STRUCT_VERSION(3)

typedef struct _NV_ENC_MAP_INPUT_RESOURCE {
    uint32_t version;
    uint32_t subResourceIndex;
    void*    inputResource;
    void*    registeredResource;
    void*    mappedResource;       // OUT
    NV_ENC_BUFFER_FORMAT mappedBufferFmt;
    uint32_t reserved1[251];
    void*    reserved2[63];
} NV_ENC_MAP_INPUT_RESOURCE;
#define NV_ENC_MAP_INPUT_RESOURCE_VER NVENCAPI_STRUCT_VERSION(4)

typedef struct _NV_ENC_PIC_PARAMS {
    uint32_t version;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t inputPitch;
    uint32_t encodePicFlags;
    uint32_t frameIdx;
    uint64_t inputTimeStamp;
    uint64_t inputDuration;
    void*    inputBuffer;
    void*    outputBitstream;
    void*    completionEvent;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    NV_ENC_PIC_STRUCT pictureStruct;
    int      pictureType;
    uint8_t  codecPicParams[1224];
    void*    meHintCountsPerBlock[2];
    void*    meExternalHints;
    uint32_t reserved1[6];
    void*    reserved2[2];
    int8_t   qpDeltaMap;
    uint32_t qpDeltaMapSize;
    uint32_t reservedBitFields;
    uint16_t meHintRefPicDist[2];
    void*    alphaBuffer;
    void*    meExternalSbHints;
    uint32_t meSbHintsCount;
    void*    pStateBufferRecon;
    void*    outputReconBuffer;
    int      outputStatsLevel;
    void*    pOutputStats;
    uint64_t outputStatsPtsArray;
    void*    pInputFencePoint;
    void*    pOutputFencePoint;
    uint32_t reserved3[238];
    void*    reserved4[59];
} NV_ENC_PIC_PARAMS;
#define NV_ENC_PIC_PARAMS_VER NVENCAPI_STRUCT_VERSION(6) | (1 << 31)

typedef struct _NV_ENC_LOCK_BITSTREAM {
    uint32_t version;
    uint32_t doNotWait : 1;
    uint32_t ltrFrame  : 1;
    uint32_t getRCStats : 1;
    uint32_t reservedBitFields : 29;
    void*    outputBitstream;
    uint32_t* sliceOffsets;
    uint32_t frameIdx;
    uint32_t hwEncodeStatus;
    uint32_t numSlices;
    uint32_t bitstreamSizeInBytes;
    uint64_t outputTimeStamp;
    uint64_t outputDuration;
    void*    bitstreamBufferPtr;
    int      pictureType;
    int      pictureStruct;
    uint32_t frameAvgQP;
    uint32_t frameSatd;
    uint32_t ltrFrameIdx;
    uint32_t ltrFrameBitmap;
    uint32_t temporalLayerId;
    uint32_t reserved[12];
    uint32_t intraMBCount;
    uint32_t interMBCount;
    int32_t  averageMVX;
    int32_t  averageMVY;
    uint32_t alphaLayerSizeInBytes;
    uint32_t outputStatsPtrSize;
    void*    outputStatsPtr;
    uint32_t reserved1[219];
    void*    reserved2[64];
} NV_ENC_LOCK_BITSTREAM;
#define NV_ENC_LOCK_BITSTREAM_VER NVENCAPI_STRUCT_VERSION(2)

typedef NVENCSTATUS (NVENCAPI* PFN_NvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*, void**);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncInitializeEncoder)  (void*, NV_ENC_INITIALIZE_PARAMS*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncDestroyEncoder)     (void*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncGetEncodePresetConfigEx)(void*, GUID2, GUID2, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncCreateBitstreamBuffer)(void*, NV_ENC_CREATE_BITSTREAM_BUFFER*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncDestroyBitstreamBuffer)(void*, void*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncRegisterResource)(void*, NV_ENC_REGISTER_RESOURCE*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncUnregisterResource)(void*, void*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncMapInputResource)(void*, NV_ENC_MAP_INPUT_RESOURCE*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncUnmapInputResource)(void*, void*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncEncodePicture)(void*, NV_ENC_PIC_PARAMS*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncLockBitstream)(void*, NV_ENC_LOCK_BITSTREAM*);
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncUnlockBitstream)(void*, void*);
typedef const char* (NVENCAPI* PFN_NvEncGetLastErrorString)(void*);

typedef struct _NV_ENCODE_API_FUNCTION_LIST {
    uint32_t version;
    uint32_t reserved;
    PFN_NvEncOpenEncodeSessionEx        nvEncOpenEncodeSessionEx;
    void*    pad_GetEncodeGUIDCount;
    void*    pad_GetEncodeProfileGUIDCount;
    void*    pad_GetEncodeProfileGUIDs;
    void*    pad_GetEncodeGUIDs;
    void*    pad_GetInputFormatCount;
    void*    pad_GetInputFormats;
    void*    pad_GetEncodeCaps;
    void*    pad_GetEncodePresetCount;
    void*    pad_GetEncodePresetGUIDs;
    void*    pad_GetEncodePresetConfig;
    PFN_NvEncInitializeEncoder          nvEncInitializeEncoder;
    void*    pad_CreateInputBuffer;
    void*    pad_DestroyInputBuffer;
    PFN_NvEncCreateBitstreamBuffer      nvEncCreateBitstreamBuffer;
    PFN_NvEncDestroyBitstreamBuffer     nvEncDestroyBitstreamBuffer;
    PFN_NvEncEncodePicture              nvEncEncodePicture;
    PFN_NvEncLockBitstream              nvEncLockBitstream;
    PFN_NvEncUnlockBitstream            nvEncUnlockBitstream;
    void*    pad_LockInputBuffer;
    void*    pad_UnlockInputBuffer;
    void*    pad_GetEncodeStats;
    void*    pad_GetSequenceParams;
    void*    pad_RegisterAsyncEvent;
    void*    pad_UnregisterAsyncEvent;
    void*    pad_MapInputResource_old;
    void*    pad_UnmapInputResource_old;
    PFN_NvEncDestroyEncoder             nvEncDestroyEncoder;
    void*    pad_InvalidateRefFrames;
    void*    pad_OpenEncodeSession;
    void*    pad_MapInputResource_old2;
    void*    pad_UnmapInputResource_old2;
    PFN_NvEncRegisterResource           nvEncRegisterResource;
    PFN_NvEncUnregisterResource         nvEncUnregisterResource;
    PFN_NvEncMapInputResource           nvEncMapInputResource;
    PFN_NvEncUnmapInputResource         nvEncUnmapInputResource;
    void*    pad_ReconfigureEncoder;
    void*    pad_Reserved1;
    void*    pad_CreateMVBuffer;
    void*    pad_DestroyMVBuffer;
    void*    pad_RunMotionEstimationOnly;
    PFN_NvEncGetLastErrorString         nvEncGetLastErrorString;
    void*    pad_SetIOCudaStreams;
    PFN_NvEncGetEncodePresetConfigEx    nvEncGetEncodePresetConfigEx;
    void*    pad_GetSequenceParamEx;
    void*    pad_RestoreEncoderState;
    void*    pad_LookaheadPicture;
    void*    reserved_extra[256];
} NV_ENCODE_API_FUNCTION_LIST;
#define NV_ENCODE_API_FUNCTION_LIST_VER NVENCAPI_STRUCT_VERSION(2)

typedef NVENCSTATUS (NVENCAPI* PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
