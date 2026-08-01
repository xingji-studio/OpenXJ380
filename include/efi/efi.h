// 版权所有©XINGJI Studios 2017-2026 保留所有权利。
// XJ380UEFI引导头文件
#ifndef _EFI_H_
#define _EFI_H_

#include <stdint.h>

#pragma pack(push, 8)

typedef uint8_t BOOLEAN;

#define PT_LOAD 1 // PT_LOAD值不变

enum EFI_ALLOCATE_TYPE
{
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
};
typedef uint64_t EFI_STATUS;
typedef void     VOID;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;

typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;

#define EFI_ERROR_MASK           0x8000000000000000
#define EFIERR(a)                (EFI_ERROR_MASK | a)
#define EFI_ERROR(status)        (((long long)status) < 0)
#define EFIAPI                   __attribute__((ms_abi))
#define EFI_SUCCESS              0
#define EFI_LOAD_ERROR           EFIERR(1)
#define EFI_INVALID_PARAMETER    EFIERR(2)
#define EFI_UNSUPPORTED          EFIERR(3)
#define EFI_BAD_BUFFER_SIZE      EFIERR(4)
#define EFI_BUFFER_TOO_SMALL     EFIERR(5)
#define EFI_NOT_READY            EFIERR(6)
#define EFI_DEVICE_ERROR         EFIERR(7)
#define EFI_WRITE_PROTECTED      EFIERR(8)
#define EFI_OUT_OF_RESOURCES     EFIERR(9)
#define EFI_VOLUME_CORRUPTED     EFIERR(10)
#define EFI_VOLUME_FULL          EFIERR(11)
#define EFI_NO_MEDIA             EFIERR(12)
#define EFI_MEDIA_CHANGED        EFIERR(13)
#define EFI_NOT_FOUND            EFIERR(14)
#define EFI_ACCESS_DENIED        EFIERR(15)
#define EFI_NO_RESPONSE          EFIERR(16)
#define EFI_NO_MAPPING           EFIERR(17)
#define EFI_TIMEOUT              EFIERR(18)
#define EFI_NOT_STARTED          EFIERR(19)
#define EFI_ALREADY_STARTED      EFIERR(20)
#define EFI_ABORTED              EFIERR(21)
#define EFI_ICMP_ERROR           EFIERR(22)
#define EFI_TFTP_ERROR           EFIERR(23)
#define EFI_PROTOCOL_ERROR       EFIERR(24)
#define EFI_INCOMPATIBLE_VERSION EFIERR(25)
#define EFI_SECURITY_VIOLATION   EFIERR(26)
#define EFI_CRC_ERROR            EFIERR(27)
#define EFI_END_OF_MEDIA         EFIERR(28)
#define EFI_END_OF_FILE          EFIERR(31)
#define EFI_INVALID_LANGUAGE     EFIERR(32)
#define EFI_COMPROMISED_DATA     EFIERR(33)

#define EFI_FILE_MODE_READ_ONLY 0x00000001
#define EFI_READ_ONLY           EFI_FILE_MODE_READ_ONLY

#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544e5552
#define EFI_RUNTIME_SERVICES_REVISION  EFI_SPECIFICATION_VERSIONs

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) < (b) ? (b) : (a))
struct EFI_INPUT_KEY
{
    uint16_t ScanCode;
    CHAR16   UnicodeChar;
};

typedef struct
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL       0x00000004
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008
#define EFI_OPEN_PROTOCOL_BY_DRIVER           0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE           0x00000020

enum EFI_MEMORY_TYPE
{
    EfiReservedMemoryType,      // 0 绝对不要动
    EfiLoaderCode,              // 1 绝对不要动
    EfiLoaderData,              // 2 绝对不要动
    EfiBootServicesCode,        // 3 退出BS后可以动
    EfiBootServicesData,        // 4 同上
    EfiRuntimeServicesCode,     // 5 绝对不要动
    EfiRuntimeServicesData,     // 6 绝对不要动
    EfiConventionalMemory,      // 7 随便动
    EfiUnusableMemory,          // 8 不要动, 有问题
    EfiACPIReclaimMemory,       // 9 解析ACPI表前不能动，解析后可回收
    EfiACPIMemoryNVS,           // A 绝对不要动
    EfiMemoryMappedIO,          // B 绝对不要动
    EfiMemoryMappedIOPortSpace, // C 绝对不要动
    EfiPalCode,                 // D Use of 处理器  不可以
    EfiPersistentMemory,        // E 持久内存，你想干什么?? ramfs??
    EfiMaxMemoryType           // F 枚举上限
    // 15(F没有一点用) ~ 0xFFFFFFFF 保留
    // 0x70000000 ~ 0x7FFFFFFF  OEM
    // 0x80000000 ~ 0xFFFFFFFF IDK
};

typedef enum
{
    EfiResetCold,            // 冷重启，断电再通电
    EfiResetWarm,            // 热重启，初始化为最初状态
    EfiResetShutdown,        // 软关机，置于ACPI G2、S5（软关机）或G3（断电）状态
    EfiResetPlatformSpecific // 非正常冷重启，要传入一段数据
} EFI_RESET_TYPE;

// 36字节大小

typedef struct
{
    uint32_t             Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t             NumberOfPages;
    uint64_t             Attribute;
} __attribute__((__aligned__(16))) EFI_MEMORY_DESCRIPTOR;

typedef struct
{
    uint16_t Year;   // 1900 – 9999
    uint8_t  Month;  // 1 – 12
    uint8_t  Day;    // 1 – 31
    uint8_t  Hour;   // 0 – 23
    uint8_t  Minute; // 0 – 59
    uint8_t  Second; // 0 – 59
    uint8_t  Pad1;
    uint32_t Nanosecond; // 0 – 999,999,999
    int16_t  TimeZone;   // -1440 to 1440 or 2047
    uint8_t  Daylight;
    uint8_t  Pad2;
} EFI_TIME;

typedef struct
{
    uint32_t Resolution;
    uint32_t Accuracy;
    BOOLEAN  SetsToZero;
} EFI_TIME_CAPABILITIES;

typedef struct
{
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct
{
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

struct EFI_BOOT_SERVICES
{
    char _buf1[24];

    // Task Priority Services
    uint64_t _buf2[2];

    // Memory Services
    EFIAPI
    EFI_STATUS(*AllocatePages)
    (enum EFI_ALLOCATE_TYPE Type, enum EFI_MEMORY_TYPE MemoryType, uint64_t Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFIAPI EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, uint64_t Pages);
    EFIAPI EFI_STATUS (*GetMemoryMap)(uint64_t *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, uint64_t *MapKey,
                                      uint64_t *DescriptorSize, uint32_t *DescriptorVersion);
    EFIAPI EFI_STATUS (*AllocatePool)(enum EFI_MEMORY_TYPE PoolType, uint64_t Size, void **Buffer);
    EFIAPI EFI_STATUS (*FreePool)(void *Buffer);

    // Event & Timer Services
    uint64_t _buf4[2];
    EFIAPI
    EFI_STATUS (*WaitForEvent)(uint64_t NumberOfEvents, EFI_EVENT *Event, uint64_t *Index);
    uint64_t _buf4_2[3];

    // Protocol Handler Services
    uint64_t _buf5[9];

    // Image Services
    uint64_t _buf6[4];
    EFIAPI   EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, uint64_t MapKey);

    // Miscellaneous Services
    uint64_t _buf7[1];
    EFIAPI   EFI_STATUS (*Stall)(uint64_t Microseconds);
    EFIAPI   EFI_STATUS (*SetWatchdogTimer)(uint64_t Timeout, uint64_t WatchdogCode, uint64_t DataSize,
                                          CHAR16 *WatchdogData);

    // DriverSupport Services
    uint64_t _buf8[2];

    // Open and Close Protocol Services
    EFIAPI
    EFI_STATUS(*OpenProtocol)
    (EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle,
     uint32_t Attributes);
    uint64_t _buf9[2];

    // Library Services
    uint64_t _buf10[2];
    EFIAPI
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    uint64_t _buf10_2[2];

    // 32-bit CRC Services
    uint64_t _buf11;

    // Miscellaneous Services
    uint64_t _buf12[3];
};

struct EFI_RUNTIME_SERVICES
{
    EFI_TABLE_HEADER Hdr;
    // Variable Services
    uint64_t _buf2[4];

    // Time Services
    EFIAPI   EFI_STATUS (*GetTime)(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Capabilities);
    uint64_t _buf3[3];

    // Virtual Memory Services
    uint64_t _buf4[2];

    EFIAPI EFI_STATUS (*GetNextHighMonotonicCount)(uint32_t *HighCount);

    EFIAPI void (*ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus, uint64_t DataSize, void *ResetData);
    uint64_t _buf5[3];
};

struct EFI_SYSTEM_TABLE
{
    EFI_TABLE_HEADER Hdr;
    CHAR16          *FirmwareVendor;
    uint32_t         FirmwareRevision;
    EFI_HANDLE       ConsoleInHandle;
    struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL
    {
        uint64_t  Reset;
        EFIAPI    EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, struct EFI_INPUT_KEY *Key);
        EFI_EVENT WaitForKey;
    }                *ConIn;
    EFI_HANDLE        ConsoleOutHandle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
    {
        uint64_t _buf;
        EFIAPI
        EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
        uint64_t _buf2[3];
        EFIAPI   EFI_STATUS (*SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, uint64_t Attribute);
        EFIAPI   EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    }                           *ConOut;
    EFI_HANDLE                   StandardErrorHandle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    struct EFI_RUNTIME_SERVICES *RuntimeServices; // 此有bug
    struct EFI_BOOT_SERVICES    *BootServices;
    UINTN                        NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE     *ConfigurationTable;
};
struct EFI_SIMPLE_POINTER_STATE; // 用之前声明
// 简单（鼠标）指针协议
struct EFI_SIMPLE_POINTER_PROTOCOL
{
    EFIAPI EFI_STATUS (*Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *This, unsigned char ExtendedVerification);
    EFIAPI EFI_STATUS (*GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *This, struct EFI_SIMPLE_POINTER_STATE *State);
    void  *WaitForInput;
};

enum EFI_BATTERY_CHARGING_STATUS
{
    EfiBatteryChargingStatusNone,
    EfiBatteryChargingStatusSuccess,
    EfiBatteryChargingStatusOverheat,
    EfiBatteryChargingStatusVoltageOutOfRange,
    EfiBatteryChargingStatusCurrentOutOfRange,
    EfiBatteryChargingStatusTimeout,
    EfiBatteryChargingStatusAborted,
    EfiBatteryChargingStatusDeviceError,
    EfiBatteryChargingStatusExtremeCold,
    EfiBatteryChargingStatusBatteryChargingNotSupported,
    EfiBatteryChargingStatusBatteryNotDetected,
    EfiBatteryChargingSourceNotDetected,
    EfiBatteryChargingSourceVoltageInvalid,
    EfiBatteryChargingSourceCurrentInvalid,
    EfiBatteryChargingErrorRequestShutdown,
    EfiBatteryChargingErrorRequestReboot
};

struct EFI_BATTERY_CHARGING_COMPLETION_TOKEN
{
    EFI_EVENT                        Event;
    enum EFI_BATTERY_CHARGING_STATUS Status;
};

struct EFI_BATTERY_CHARGING_PROTOCOL
{
    EFIAPI   EFI_STATUS (*GetBatteryStatus)(struct EFI_BATTERY_CHARGING_PROTOCOL *This, uint32_t *StateOfCharge,
                                          uint32_t *RatedCapacity, int32_t *ChargeCurrent);
    EFIAPI   EFI_STATUS (*ChargeBattery)(struct EFI_BATTERY_CHARGING_PROTOCOL *This, uint32_t MaximumCurrent,
                                       uint32_t                                      TargetStateOfCharge,
                                       struct EFI_BATTERY_CHARGING_COMPLETION_TOKEN *CompletionToken);
    uint32_t Revision;
    EFIAPI   EFI_STATUS (*Get_Battery_Information)(struct EFI_BATTERY_CHARGING_PROTOCOL *This, uint32_t *StateOfCharge,
                                                 int32_t *CurrentIntoBattery, uint32_t *BatteryTerminalVoltage,
                                                 int32_t *BatteryTemperature, uint32_t *USBCableVoltage,
                                                 uint32_t *USBCableCurrent);
};

struct EFI_SIMPLE_POINTER_STATE
{
    int32_t RelativeMovementX; /* X轴方向的相对移动量 */
    int32_t RelativeMovementY; /* Y轴方向的相对移动量 */
    int32_t RelativeMovementZ; /* Z轴方向的相对移动量 */
    BOOLEAN LeftButton;        /* 左键状态，按下为1，松开为0 */
    BOOLEAN RightButton;       /* 右键状态，同上 */
};

struct EFI_GRAPHICS_OUTPUT_BLT_PIXEL
{
    unsigned char Blue;
    unsigned char Green;
    unsigned char Red;
    unsigned char Reserved;
};

typedef enum
{
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBitOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct
{
    uint32_t                  Version;
    uint32_t                  HorizontalResolution;
    uint32_t                  VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    uint32_t                  PixelInformation[4];
    uint32_t                  PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct
{
    uint32_t                              MaxMode;
    uint32_t                              Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    uint64_t                              SizeOfInfo;
    uint64_t                              FrameBufferBase;
    uint64_t                              FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL
{
    EFIAPI EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, unsigned int ModeNumber,
                                   unsigned long long *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

    EFIAPI EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, unsigned int ModeNumber);

    uint64_t pad;

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct EFI_KEY_DATA; // 用之前声明
struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL
{
    /* 重置输入设备 */
    unsigned long long (*Reset)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, unsigned char ExtendedVerification);

    /* 获取按键输入数据 */
    unsigned long long (*ReadKeyStrokeEx)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, struct EFI_KEY_DATA *KeyData);

    /* 等待按键输入的事件，EFI_EVENT类型 */
    void *WaitForKeyEx;

    /* 设置输入设备状态(NumLock、CapsLock等) */
    unsigned long long (*SetState)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, unsigned char *KeyToggleState);

    /* 绑定按键事件处理函数 */
    unsigned long long (*RegisterKeyNotify)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                            struct EFI_KEY_DATA                      *KeyData,
                                            unsigned long long (*KeyNotificationFunction)(struct EFI_KEY_DATA *KeyData),
                                            void **NotifyHandle);

    /* 解绑按键事件 */
    unsigned long long (*UnregisterKeyNotify)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *NotificationHandle);
};

struct EFI_LOADED_IMAGE_PROTOCOL
{
    uint32_t                 Revision;
    EFI_HANDLE               ParentHandle;
    struct EFI_SYSTEM_TABLE *SystemTable;

    EFI_HANDLE DeviceHandle;
    void *
        FilePath; // EFI_DEVICE_PATH_PROTOCOL我们并不会用到，除非我哪天脑子一闪想要把执行EFI的内容加进来，如果真加到时候就改
    void *Reserved;

    uint32_t LoadOptionsSize;
    void    *LoadOptions;

    void                *ImageBase;
    uint64_t             ImageSize;
    enum EFI_MEMORY_TYPE ImageCodeType;
    enum EFI_MEMORY_TYPE ImageDataType; // 这个enum后面确实会用到，但此处先不加
    EFIAPI               EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
}; // LIP

struct EFI_FILE_INFO
{
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    uint64_t Attribute;
    CHAR16   FileName[];
};

#define EFI_FILE_MODE_READ   0x0000000000000001
#define EFI_FILE_MODE_WRITE  0x0000000000000002
#define EFI_FILE_MODE_CREATE 0x8000000000000000

struct EFI_FILE_PROTOCOL
{
    uint64_t Revision;
    EFIAPI
    EFI_STATUS(*Open)
    (struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, uint64_t OpenMode,
     uint64_t Attributes);
    EFIAPI   EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *This);
    EFIAPI   EFI_STATUS (*Delete)(struct EFI_FILE_PROTOCOL *This);
    EFIAPI
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *This, uint64_t *BufferSize, void *Buffer);
    EFIAPI
    EFI_STATUS (*Write)(struct EFI_FILE_PROTOCOL *This, uint64_t *BufferSize, void *Buffer);
    EFIAPI EFI_STATUS (*GetPosition)(struct EFI_FILE_PROTOCOL *This, uint64_t *Position);
    EFIAPI EFI_STATUS (*SetPosition)(struct EFI_FILE_PROTOCOL *This, uint64_t Position);
    EFIAPI EFI_STATUS (*GetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, uint64_t *BufferSize,
                                 void *Buffer);
    EFIAPI EFI_STATUS (*SetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, uint64_t BufferSize,
                                 void *Buffer);
    EFIAPI   EFI_STATUS (*Flush)(struct EFI_FILE_PROTOCOL *This);
}; // EFI_FILE_PROTOCOL

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    uint64_t Revision;
    EFIAPI   EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, struct EFI_FILE_PROTOCOL **Root);
}; // SFSP

#pragma pack(pop)
#endif
