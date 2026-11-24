PC preview of the 3DS Maps screens. `3ds.h` here is a tiny stand-in for libctru,
so `source/main.c` can be compiled natively and its screens saved as PNGs in `out/`.

`font.bin` is libctru's 8x8 console font, extracted with:

    arm-none-eabi-ar x $DEVKITPRO/libctru/lib/libctru.a default_font.bin.o
    arm-none-eabi-objcopy -O binary -j .rodata.default_font_bin default_font.bin.o font.bin

Run: `make -C tests/preview` (needs libjansson-dev and Python Pillow).
