#ifndef _XJ380_FAT_H_
#define _XJ380_FAT_H_

#define ADR_DISKIMG		0x00100000

struct FILEINFO{
	unsigned char name[8], ext[3], type;
	char reserve[10];
	unsigned short time, date, clustno;
	unsigned int size;
};

struct FILEDATA{
	char data[32];
};

#endif