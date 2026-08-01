#ifndef _ACPI_H_
#define _ACPI_H_

#pragma pack(push, 1)
#ifdef __cplusplus
extern "C" {
#endif
#include <efi/efi.h>
#ifdef __cplusplus
}
#endif

typedef struct
{
    char     sign[8];   // 标志位“RSD PTR ”（注意空白字符）
    uint8_t  checksum1; // 0~19字节的校验和（我也不知道怎么算的但我们大抵用不着）
    char     OEMID[6];  // 用于识别OEM的字符串
    uint8_t  version;   // ACPI版本（注意：ACPI 1.0为0）
    uint32_t RsdtAddr;  // RSDT的地址
    // 下面是ACPI 2.0以后的玩意儿
    uint32_t length;      // 表的长度，单位字节
    uint64_t XsdtAddr;    // XSDR的地址
    uint8_t  checksum2;   // 全表的校验和
    uint8_t  reserved[3]; // 保留
} __attribute__((packed)) RSDP_TYPE;

typedef struct
{
    char     sign[4];  // 标头
    uint32_t length;   // 长度
    uint8_t  version;  // 版本
    uint8_t  Checksum; // 校验和
    char     OEMID[6];
    char     OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__((packed)) ACPI_TABLE_HEADER;

struct ACPISDTHeader
{
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OEMID[6];
    char     OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
};

#pragma pack(pop)

#endif