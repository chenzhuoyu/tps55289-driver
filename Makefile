MCU     = attiny1614
FREQ	= 20000000
PROG    = serialupdi
PORT    = /dev/cu.usbserial-110

TARGET  = tps55289
OBJECTS = main.o

CC		= /opt/homebrew/opt/avr-gcc@15/bin/avr-gcc
SIZE	= /opt/homebrew/bin/avr-size
OBJCOPY = /opt/homebrew/bin/avr-objcopy
AVRDUDE = /opt/homebrew/bin/avrdude

CFLAGS  = -std=c23 -Wall -Os -Wextra -fdata-sections -ffunction-sections -DF_CPU=${FREQ}
LDFLAGS = -flto -Wl,--gc-sections -Wl,--print-gc-sections -Wl,-Map,${TARGET}.map

.PHONY: all flash clean

all: size

clean:
	rm -vrf ${TARGET}.elf ${TARGET}.hex ${TARGET}.map *.o

size: ${TARGET}.elf
	${SIZE} --format=avr --mcu=${MCU} ${TARGET}.elf

flash: ${TARGET}.hex
	${AVRDUDE} -c ${PROG} -P ${PORT} -B 345600 -p ${MCU} -U flash:w:${TARGET}.hex

${TARGET}.hex: ${TARGET}.elf
	${OBJCOPY} -R .eeprom -R .fuse -R .lock -R .signature -O ihex ${TARGET}.elf ${TARGET}.hex

${TARGET}.elf: ${OBJECTS}
	${CC} -mmcu=${MCU} ${LDFLAGS} -o ${TARGET}.elf ${OBJECTS}

%.o: %.c
	${CC} -mmcu=${MCU} ${CFLAGS} -c -o $@ $<
