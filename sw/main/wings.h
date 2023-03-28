#ifndef WINGS_H
#define WINGS_H

void wings_init(void);
void wings_open(bool open);
void wings_scan(bool on);
bool wings_opened(void);
bool wings_closed(void);
void wings_tick(void);

#endif
