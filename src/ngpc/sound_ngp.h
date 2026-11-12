/* sound_ngp.h - the game's music and sound effects on Paula (src/ngpc/sound_ngp.c). */
#ifndef SOUND_NGP_H
#define SOUND_NGP_H
#include <exec/types.h>
int  sound_load(void);                  /* before sys_take(): dist/sound.bin (optional) and the waveforms in chip RAM */
void sound_start(void);                 /* after sys_take(): Paula on, the driver model's commands come here */
void sound_vbl(void);                   /* every vertical blank (from sys_vbl_hook) */
void sound_cmd(UBYTE cmd);              /* a command the game sent its sound program */
void sound_stop(void);                  /* before sys_give() */
extern ULONG sound_missing;             /* commands no recorded stream answers */
#endif
