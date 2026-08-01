#ifndef _ACPI_H_
#define _ACPI_H_

#pragma pack(1)
#ifdef __cplusplus
extern "C"
{
#endif
#include <efi.h>
#ifdef __cplusplus
}
#endif

typedef struct
{
    char sign[8];    // 标志位“RSD PTR ”（注意空白字符）
    UINT8 checksum1; // 0~19字节的校验和（我也不知道怎么算的但我们大抵用不着）
    char OEMID[6];   // 用于识别OEM的字符串
    UINT8 version;   // ACPI版本（注意：ACPI 1.0为0）
    UINT32 RsdtAddr; // RSDT的地址
    // 下面是ACPI 2.0以后的玩意儿
    UINT32 length;     // 表的长度，单位字节
    UINT64 XsdtAddr;   // XSDR的地址
    UINT8 checksum2;   // 全表的校验和
    UINT8 reserved[3]; // 保留
} __attribute__((packed)) RSDP_TYPE;

typedef struct
{
    char sign[4];   // 标头
    UINT32 length;  // 长度
    UINT8 version;  // 版本
    UINT8 Checksum; // 校验和
    char OEMID[6];
    char OEMTableID[8];
    UINT32 OEMRevision;
    UINT32 CreatorID;
    UINT32 CreatorRevision;
} __attribute__((packed)) ACPI_TABLE_HEADER;

struct ACPISDTHeader
{
    char Signature[4];
    UINT32 Length;
    UINT8 Revision;
    UINT8 Checksum;
    char OEMID[6];
    char OEMTableID[8];
    UINT32 OEMRevision;
    UINT32 CreatorID;
    UINT32 CreatorRevision;
};

#pragma pack()

#endif