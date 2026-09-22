#pragma once
#include <stdbool.h>
typedef void (*summon_receive_fn)(const char *json);
void summon_network_init(summon_receive_fn receive);
bool summon_network_online(void);
bool summon_network_enabled(void);
bool summon_network_send(const char *json);
void summon_network_enable(void);
bool summon_network_set_token(const char *token);
