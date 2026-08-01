#include <proto.hpp>
#include <pci/pci.h>
#include <cpu/longm.h>

// PC Speaker端口
#define PIT_CHANNEL2    0x42
#define PIT_CMD         0x43
#define PC_SPEAKER_PORT 0x61

// PIT频率
#define PIT_FREQUENCY 1193180

static int pcspk_initialized = 0;
[[deprecated]]
void pcspk_play(uint32_t frequency) {
    if (!pcspk_initialized) {
        write_serial_string("PC Speaker not initialized\n");
        return;
    }
    
    if (frequency == 0) {
        // 停止播放
        uint8_t tmp = inb(PC_SPEAKER_PORT) & 0xFC;
        outb(PC_SPEAKER_PORT, tmp);
        return;
    }
    
    // 设置PIT通道2的频率
    uint32_t divisor = PIT_FREQUENCY / frequency;
    
    // 配置PIT
    outb(PIT_CMD, 0xB6); // 通道2，方波，二进制
    outb(PIT_CHANNEL2, divisor & 0xFF);
    outb(PIT_CHANNEL2, (divisor >> 8) & 0xFF);
    
    // 打开扬声器
    uint8_t tmp = inb(PC_SPEAKER_PORT);
    if (tmp != (tmp | 3)) {
        outb(PC_SPEAKER_PORT, tmp | 3);
    }
    
    write_serial_fmt("PC Speaker: Playing frequency %d Hz\n", frequency);
}
[[deprecated]]
void pcspk_stop() {
    uint8_t tmp = inb(PC_SPEAKER_PORT) & 0xFC;
    outb(PC_SPEAKER_PORT, tmp);
    write_serial_string("PC Speaker: Stopped\n");
}
[[deprecated]]
int pcspk_init() {
    // 确保扬声器被禁用
    pcspk_stop();
    pcspk_initialized = 1;
    write_serial_string("PC Speaker: Initialized\n");
    return 0;
}

// 播放简单的音阶（测试用）
[[deprecated]]
void pcspk_test() {
    if (!pcspk_initialized) {
        pcspk_init();
    }
    
    uint32_t notes[] = {262, 294, 330, 349, 392, 440, 494, 523}; // C大调音阶
    int note_count = sizeof(notes) / sizeof(notes[0]);
    
    for (int i = 0; i < note_count; i++) {
        pcspk_play(notes[i]);
        delay_s_hp(1);
    }
    
    pcspk_stop();
    write_serial_string("PC Speaker: Test completed\n");
}