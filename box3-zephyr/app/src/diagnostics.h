#ifndef BOX3_DIAGNOSTICS_H
#define BOX3_DIAGNOSTICS_H

#include <stddef.h>

void radio_summary(char *buf, size_t len);
void audio_summary(char *buf, size_t len);
void dock_summary(char *buf, size_t len);

#endif
