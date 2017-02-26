EE_BIN = fsck.elf

EE_IOP_OBJS = SIO2MAN_irx.o PADMAN_irx.o POWEROFF_irx.o DEV9_irx.o IOMANX_irx.o FILEXIO_irx.o ATAD_irx.o HDD_irx.o PFS_irx.o FSCK_irx.o
EE_RES_OBJS = background.o pad_layout.o
EE_OBJS = main.o iop.o pad.o UI.o menu.o system.o graphics.o font.o $(EE_IOP_OBJS) $(EE_RES_OBJS)

EE_INCS := -I$(PS2SDK)/ports/include -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I.
EE_LDFLAGS := -L$(PS2SDK)/ports/lib -L$(PS2SDK)/ee/lib -s
EE_LIBS := -lpng -lz -lm -lfreetype -lpatches -lfileXio -lpadx -lgs -lc -lkernel
EE_GPVAL = -G8192
EE_CFLAGS += -mgpopt $(EE_GPVAL)

%.o : %.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

%.o : %.s
	$(EE_AS) $(EE_ASFLAGS) $< -o $@

%.o : %.S
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(EE_BIN) : $(EE_OBJS) $(PS2SDK)/ee/startup/crt0.o
	$(EE_CC) -nostartfiles -Tlinkfile $(EE_LDFLAGS) \
		-o $(EE_BIN) $(PS2SDK)/ee/startup/crt0.o $(EE_OBJS) $(EE_LIBS)
	ee-strip -s -d -R .mdebug.eabi64 -R .reginfo -R .comment $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

background.o:
	bin2o $(EE_GPVAL) resources/background.png background.o background

pad_layout.o:
	bin2o $(EE_GPVAL) resources/pad_layout.png pad_layout.o pad_layout

SIO2MAN_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/freesio2.irx SIO2MAN_irx.o SIO2MAN_irx

PADMAN_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/freepad.irx PADMAN_irx.o PADMAN_irx

IOMANX_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/iomanX.irx IOMANX_irx.o IOMANX_irx

FILEXIO_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/fileXio.irx FILEXIO_irx.o FILEXIO_irx

POWEROFF_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/poweroff.irx POWEROFF_irx.o POWEROFF_irx

DEV9_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/ps2dev9.irx DEV9_irx.o DEV9_irx

ATAD_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/ps2atad.irx ATAD_irx.o ATAD_irx

HDD_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/ps2hdd.irx HDD_irx.o HDD_irx

PFS_irx.o:
	bin2o $(EE_GPVAL) $(PS2SDK)/iop/irx/ps2fs.irx PFS_irx.o PFS_irx

FSCK_irx.o:
	bin2o $(EE_GPVAL) fsck.irx FSCK_irx.o FSCK_irx

include $(PS2SDK)/samples/Makefile.pref
