#ifndef MENU_H
#define MENU_H

#include "lcd.h"
#include "key.h"

void menu_init(lcd *plcd);
void menu_process(key_event_t evt);

#endif /* MENU_H */
