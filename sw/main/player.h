#ifndef _PLAYER_H
#define _PLAYER_H

void player_init(void);
void *player_play(const char *name);
void player_close_stream(void *stream);
bool player_is_playing(void *stream);

#endif
