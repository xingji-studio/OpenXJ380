ENTRYPOINT = 0x30400

TOOLS = ./tools

ASM  = $(TOOLS)/nasm/nasm.exe
CC   = $(TOOLS)/i686-elf-tools-windows/bin/i686-elf-gcc.exe
LD   = $(TOOLS)/i686-elf-tools-windows/bin/i686-elf-ld.exe
EDIMG = $(TOOLS)/edimg.exe
QEMU = $(TOOLS)/qemu/qemu-system-i386.exe
QEMU_IMG = $(TOOLS)/qemu/qemu-img.exe
DD = $(TOOLS)/dd-0.6beta3/dd.exe  

ASMBFLAGS = -I boot/include/
ASMKFLAGS = -I include/ -f elf
CFLAGS    = -I include/ -c -fno-builtin -nostdlib -nostdinc
LDFLAGS   = -s -Ttext $(ENTRYPOINT)

XJ380BOOT = out/boot.bin out/loader.bin
XJ380KERN = out/kernel.bin
OBJS      = out/kernel.o out/start.o out/main.o out/string.o out/global.o out/rect.o out/text.o out/font.o out/kliba.o \
		out/klib.o out/intr.o out/i8259.o out/gdt.o out/clock.o out/syscall.o out/syscall_impl.o out/proc.o out/keyboard.o \
		out/keymap.o out/fifo.o out/printf.o out/mouse_pointer.o out/mouse.o out/sheet.o out/vram.o out/window.o \
		out/console.o out/system_api.o out/memory.o

default : clear run

everything : $(XJ380BOOT) $(XJ380KERN)

image : everything clean
	$(QEMU_IMG) create XJ380.img 1474560
	$(DD) if=out/boot.bin of=XJ380.img bs=512 count=1
	$(EDIMG) \
	imgin:.\XJ380.img \
	copy from:out/loader.bin to:@: \
	copy from:out/kernel.bin to:@: \
	imgout:XJ380.img

clean :
	del out\*.o

clear :
	del out\*.bin

run : image
	$(QEMU) -fda XJ380.img -serial stdio -m 256

out/boot.bin : boot/boot.asm
	$(ASM) $(ASMBFLAGS) -o $@ $<

out/loader.bin : boot/loader.asm
	$(ASM) $(ASMBFLAGS) -o $@ $<

$(XJ380KERN) : $(OBJS)
	$(LD) $(LDFLAGS) -o $(XJ380KERN) $(OBJS)

out/%.o : kernel/%.asm
	$(ASM) $(ASMKFLAGS) -o $@ $<

out/%.o : kernel/%.c
	$(CC) $(CFLAGS) -o $@ $<

out/%.o : lib/%.asm
	$(ASM) $(ASMKFLAGS) -o $@ $<

out/%.o : lib/%.c
	$(CC) $(CFLAGS) -o $@ $<

out/%.o : graphics/%.asm
	$(ASM) $(ASMKFLAGS) -o $@ $<

out/%.o : graphics/%.c
	$(CC) $(CFLAGS) -o $@ $<

out/%.o : drivers/%.asm
	$(ASM) $(ASMKFLAGS) -o $@ $<

out/%.o : drivers/%.c
	$(CC) $(CFLAGS) -o $@ $<

out/%.o : gui/%.asm
	$(ASM) $(ASMKFLAGS) -o $@ $<

out/%.o : gui/%.c
	$(CC) $(CFLAGS) -o $@ $<

out/%.o : api/%.c
	$(CC) $(CFLAGS) -o $@ $<
