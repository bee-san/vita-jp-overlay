#ifndef VJO_KERNEL_TEXT_H
#define VJO_KERNEL_TEXT_H
int text_init(void);
void text_shutdown(void);
int text_uses_frame_checks(void);
/* Foreground state is read on every tick; a changed PID/mode clears all sources. */
void text_tick(void);
#endif
