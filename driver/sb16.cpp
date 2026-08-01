#include "sb16.h"
#include <dma.h>
#include <proto.hpp>
#include <pctable/idt.h>

static volatile bool sig     = false;
uint64_t             buf_phy = 0;

/* sb16 interrupt handler */
extern "C" void sb16_handler(struct X64_REGS *frame, uint64_t errcode)
{
    close_interrupt;
    inb(SB_INTR16);
    send_eoi();
    sig = true;
    open_interrupt;
}

/* Send data to port sb16 */
void sb_out(uint8_t value)
{
    while (inb(SB_WRITE) & 0x80)
        ;
    outb(SB_WRITE, value);
}
extern void init_idt_desc(uint8_t number, uint8_t gate_type, int_func function, uint8_t privilege, void *data);
/* Initialize sb16 sound card */
void sb16_init(void)
{
    if (!sb_reset()) return;

    buf_phy = alloc_frames(PADDING_UP(DMA_BUF_SIZE, PAGE_SIZE) / PAGE_SIZE);

    outb(SB_MIXER, 0x80);
    outb(SB_MIXER_DATA, 0x02);

    sb_out(DSP_CMD_SPEAKER_ON);

    init_idt_desc(40,X86_64_INTR_GATE,sb16_interr,0,NULL);
}

/* Reset sb16 sound card */
bool sb_reset(void)
{
    outb(SB_RESET, 1);
    delay_us_hp(5000);
    outb(SB_RESET, 0);

    if (inb(SB_STATE) == 0x80)
    {
        write_serial_fmt("SB16: Reset OK, state = 0x%x", inb(SB_READ));
        return true;
    }
    else
    {
        return false;
    }
}

/* Set the default volume of the sb16 sound card */
void sb16_set_volume(uint8_t left, uint8_t right)
{
    if (left > 15) left = 15;
    if (right > 15) right = 15;

    outb(SB_MIXER, 0x22);
    outb(SB_MIXER_DATA, (left << 4) | (right & 0x0F));
}

/* Set the default baud rate of the sb16 sound card */
void sb16_set_sample_rate(uint16_t rate)
{
    sb_out(0x41);
    sb_out((rate >> 8) & 0xff);
    sb_out(rate & 0xff);
}

/* Send data packets to the sb16 sound card */
void sb16_send_data(const uint8_t *data, size_t size)
{
    uint16_t count = (size / 2 - 1);

    if (size > DMA_BUF_SIZE) size = DMA_BUF_SIZE;

    for (size_t i = 0; i < size; i++)
        ((uint8_t *)phys_to_virt(buf_phy))[i] = data[i];
    dma_send(5, (uint32_t *)buf_phy, size);

    sb_out(0xB0);
    sb_out(0x10);
    sb_out(count & 0xff);
    sb_out((count >> 8) & 0xff);
}

/* Play PCM audio data */
void sb16_play(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        size_t chunk = size - offset;
        if (chunk > DMA_BUF_SIZE) chunk = DMA_BUF_SIZE;
        sb16_send_data(data + offset, chunk);
        waitif(sig == false);
        sig     = false;
        offset += chunk;
    }
}
