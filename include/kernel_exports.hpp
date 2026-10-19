#pragma once
#include <cstdint>
namespace ghoulies {
// DATA ordinals from XboxDev/nxdk's kernel export definition (CC0).
// This metadata is an export classification, not an implementation of objects.
inline const char* kernel_data_name(uint32_t ordinal) {
    switch(ordinal) {
    case 16:return "ExEventObjectType"; case 22:return "ExMutantObjectType";
    case 30:return "ExSemaphoreObjectType"; case 31:return "ExTimerObjectType";
    case 40:return "HalDiskCachePartitionCount"; case 41:return "HalDiskModelNumber"; case 42:return "HalDiskSerialNumber";
    case 64:return "IoCompletionObjectType"; case 70:return "IoDeviceObjectType"; case 71:return "IoFileObjectType";
    case 88:return "KdDebuggerEnabled"; case 89:return "KdDebuggerNotPresent"; case 102:return "MmGlobalData";
    case 120:return "KeInterruptTime"; case 154:return "KeSystemTime"; case 156:return "KeTickCount"; case 157:return "KeTimeIncrement";
    case 162:return "KiBugCheckData"; case 164:return "LaunchDataPage"; case 240:return "ObDirectoryObjectType";
    case 245:return "ObpObjectHandleTable"; case 249:return "ObSymbolicLinkObjectType"; case 259:return "PsThreadObjectType";
    case 321:return "XboxEEPROMKey"; case 322:return "XboxHardwareInfo"; case 323:return "XboxHDKey";
    case 324:return "XboxKrnlVersion"; case 325:return "XboxSignatureKey"; case 326:return "XeImageFileName";
    case 353:return "XboxLANKey"; case 354:return "XboxAlternateSignatureKeys"; case 355:return "XePublicKeyData";
    case 356:return "HalBootSMCVideoMode"; case 357:return "IdexChannelObject";
    default:return nullptr;
    }
}
}
