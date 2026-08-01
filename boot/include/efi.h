// 版权所有©XINGJI Studios 2017-2026 保留所有权利。
// XJ380UEFI引导头文件
#ifndef _EFI_H_
#define _EFI_H_
// efi规范定义，不要动

#pragma pack(push, 8)
// 无符号整型
typedef __UINT8_TYPE__  UINT8;
typedef __UINT16_TYPE__ UINT16;
typedef __UINT32_TYPE__ UINT32;
typedef __UINT64_TYPE__ UINT64;
// 有符号整型
typedef __INT8_TYPE__   INT8;
typedef __INT16_TYPE__  INT16;
typedef __INT32_TYPE__  INT32;
typedef __INT64_TYPE__  INT64;
// 布尔类型
typedef char            BOOLEAN;

// 自定义类型
// 无符号整型
typedef UINT8  u8;
typedef UINT16 u16;
typedef UINT32 u32;
typedef UINT64 u64;
// 有符号整型
typedef INT8   i8;
typedef INT16  i16;
typedef INT32  i32;
typedef INT64  i64;
// 字符
typedef char   CHAR8;

typedef unsigned long long size_t;

#define PT_LOAD 1 // PT_LOAD值不变

enum EFI_ALLOCATE_TYPE
{
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
};
typedef UINT64 UINTN; // 我们在64位下
typedef UINTN  EFI_STATUS;
typedef void   VOID;
typedef UINT16 CHAR16;

typedef UINTN EFI_PHYSICAL_ADDRESS;
typedef UINTN EFI_VIRTUAL_ADDRESS;

typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;

typedef enum
{
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

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
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
};

typedef struct
{
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL       0x00000004
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008
#define EFI_OPEN_PROTOCOL_BY_DRIVER           0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE           0x00000020

enum EFI_MEMORY_TYPE
{
    EfiReservedMemoryType,      // 0 不要动
    EfiLoaderCode,              // 1 不要动
    EfiLoaderData,              // 2 不要动
    EfiBootServicesCode,        // 3 可以动
    EfiBootServicesData,        // 4 可以动
    EfiRuntimeServicesCode,     // 5 不要动
    EfiRuntimeServicesData,     // 6 不要动
    EfiConventionalMemory,      // 7 可以动
    EfiUnusableMemory,          // 8 不要动
    EfiACPIReclaimMemory,       // 9 不要动
    EfiACPIMemoryNVS,           // A 不要动
    EfiMemoryMappedIO,          // B 不要动
    EfiMemoryMappedIOPortSpace, // C 不要动
    EfiPalCode,                 // D 不知道
    EfiPersistentMemory,        // E 不知道
    EfiMaxMemoryType            // F 不知道
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
    UINT32               Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} __attribute__((__aligned__(16))) EFI_MEMORY_DESCRIPTOR;

static inline void xmemset(void *dst_, UINT8 value, UINTN size)
{
    UINT8 *dst = (UINT8 *)dst_;
    while (size-- > 0)
        *dst++ = value;
}

static inline void *xmemcpy(void *dst_, const void *src_, UINTN size)
{
    UINT8       *dst = (UINT8 *)dst_;
    const UINT8 *src = (UINT8 *)src_;
    while (size-- > 0)
        *dst++ = *src++;
    return (void *)src_;
}

typedef struct
{
    UINT16 Year;   // 1900 – 9999
    UINT8  Month;  // 1 – 12
    UINT8  Day;    // 1 – 31
    UINT8  Hour;   // 0 – 23
    UINT8  Minute; // 0 – 59
    UINT8  Second; // 0 – 59
    UINT8  Pad1;
    UINT32 Nanosecond; // 0 – 999,999,999
    INT16  TimeZone;   // -1440 to 1440 or 2047
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct
{
    UINT32  Resolution;
    UINT32  Accuracy;
    BOOLEAN SetsToZero;
} EFI_TIME_CAPABILITIES;

typedef struct
{
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct
{
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

struct EFI_BOOT_SERVICES
{
    char _buf1[24];

    // Task Priority Services
    UINTN _buf2[2];

    // Memory Services
    EFIAPI
    EFI_STATUS(*AllocatePages)
    (enum EFI_ALLOCATE_TYPE Type, enum EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFIAPI EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFIAPI EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
                                      UINTN *DescriptorSize, UINT32 *DescriptorVersion);
    EFIAPI EFI_STATUS (*AllocatePool)(enum EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
    EFIAPI EFI_STATUS (*FreePool)(VOID *Buffer);

    // Event & Timer Services
    UINTN _buf4[2];
    EFIAPI
    EFI_STATUS (*WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    UINTN _buf4_2[3];

    // Protocol Handler Services
    UINTN _buf5[9];

    // Image Services
    UINTN  _buf6[4];
    EFIAPI EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

    // Miscellaneous Services
    UINTN  _buf7[1];
    EFIAPI EFI_STATUS (*Stall)(UINTN Microseconds);
    EFIAPI EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);

    // DriverSupport Services
    UINTN _buf8[2];

    // Open and Close Protocol Services
    EFIAPI
    EFI_STATUS(*OpenProtocol)
    (EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle,
     UINT32 Attributes);
    UINTN _buf9[2];

    // Library Services
    UINTN _buf10[1];
    EFIAPI
    EFI_STATUS (*LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol, VOID *SearchKey,
                                     UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFIAPI
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
    UINTN _buf10_2[2];

    // 32-bit CRC Services
    UINTN _buf11;

    // Miscellaneous Services
    UINTN _buf12[3];
};

struct EFI_RUNTIME_SERVICES
{
    EFI_TABLE_HEADER Hdr;
    // Variable Services
    UINTN _buf2[4];

    // Time Services
    EFIAPI EFI_STATUS (*GetTime)(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Capabilities);
    UINTN  _buf3[3];

    // Virtual Memory Services
    UINTN _buf4[2];

    EFIAPI EFI_STATUS (*GetNextHighMonotonicCount)(UINT32 *HighCount);

    EFIAPI VOID (*ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus, UINTN DataSize, VOID *ResetData);
    UINTN  _buf5[3];
};

struct EFI_SYSTEM_TABLE
{
    EFI_TABLE_HEADER Hdr;
    CHAR16          *FirmwareVendor;
    UINT32           FirmwareRevision;
    EFI_HANDLE       ConsoleInHandle;
    struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL
    {
        UINTN     Reset;
        EFIAPI    EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, struct EFI_INPUT_KEY *Key);
        EFI_EVENT WaitForKey;
    }    *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
    {
        UINTN _buf;
        EFIAPI
        EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
        UINTN  _buf2[3];
        EFIAPI EFI_STATUS (*SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
        EFIAPI EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
        EFIAPI EFI_STATUS (*SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
        EFIAPI EFI_STATUS (*EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);
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
    EFIAPI EFI_STATUS (*GetBatteryStatus)(struct EFI_BATTERY_CHARGING_PROTOCOL *This, UINT32 *StateOfCharge,
                                          UINT32 *RatedCapacity, INT32 *ChargeCurrent);
    EFIAPI EFI_STATUS (*ChargeBattery)(struct EFI_BATTERY_CHARGING_PROTOCOL *This, UINT32 MaximumCurrent,
                                       UINT32                                        TargetStateOfCharge,
                                       struct EFI_BATTERY_CHARGING_COMPLETION_TOKEN *CompletionToken);
    UINT32 Revision;
    EFIAPI EFI_STATUS (*Get_Battery_Information)(struct EFI_BATTERY_CHARGING_PROTOCOL *This, UINT32 *StateOfCharge,
                                                 INT32 *CurrentIntoBattery, UINT32 *BatteryTerminalVoltage,
                                                 INT32 *BatteryTemperature, UINT32 *USBCableVoltage,
                                                 UINT32 *USBCableCurrent);
};

struct EFI_SIMPLE_POINTER_STATE
{
    INT32   RelativeMovementX; /* X轴方向的相对移动量 */
    INT32   RelativeMovementY; /* Y轴方向的相对移动量 */
    INT32   RelativeMovementZ; /* Z轴方向的相对移动量 */
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
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    UINT32                    PixelInformation[4];
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct
{
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    UINTN                                 FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL
{
    EFIAPI EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, unsigned int ModeNumber,
                                   unsigned long long *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

    EFIAPI EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, unsigned int ModeNumber);

    UINTN pad;

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
    UINT32                   Revision;
    EFI_HANDLE               ParentHandle;
    struct EFI_SYSTEM_TABLE *SystemTable;

    EFI_HANDLE DeviceHandle;
    VOID *
        FilePath; // EFI_DEVICE_PATH_PROTOCOL我们并不会用到，除非我哪天脑子一闪想要把执行EFI的内容加进来，如果真加到时候就改
    VOID *Reserved;

    UINT32 LoadOptionsSize;
    VOID  *LoadOptions;

    VOID                *ImageBase;
    UINT64               ImageSize;
    enum EFI_MEMORY_TYPE ImageCodeType;
    enum EFI_MEMORY_TYPE ImageDataType; // 这个enum后面确实会用到，但此处先不加
    EFIAPI               EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
}; // LIP

struct EFI_FILE_INFO
{
    UINT64   Size;
    UINT64   FileSize;
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[];
};

#define EFI_FILE_MODE_READ   0x0000000000000001
#define EFI_FILE_MODE_WRITE  0x0000000000000002
#define EFI_FILE_MODE_CREATE 0x8000000000000000

struct EFI_FILE_PROTOCOL
{
    UINTN Revision;
    EFIAPI
    EFI_STATUS(*Open)
    (struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode,
     UINT64 Attributes);
    EFIAPI EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *This);
    EFIAPI EFI_STATUS (*Delete)(struct EFI_FILE_PROTOCOL *This);
    EFIAPI
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
    EFIAPI
    EFI_STATUS (*Write)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
    EFIAPI EFI_STATUS (*GetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFIAPI EFI_STATUS (*SetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 Position);
    EFIAPI EFI_STATUS (*GetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize,
                                 VOID *Buffer);
    EFIAPI EFI_STATUS (*SetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize,
                                 VOID *Buffer);
    EFIAPI EFI_STATUS (*Flush)(struct EFI_FILE_PROTOCOL *This);
}; // EFI_FILE_PROTOCOL

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    UINT64 Revision;
    EFIAPI EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, struct EFI_FILE_PROTOCOL **Root);
}; // SFSP

#pragma pack(pop)
#endif
