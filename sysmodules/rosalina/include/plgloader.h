#pragma once

#include <3ds/types.h>
#include "MyThread.h"

void        PluginLoader__Init(void);
bool        PluginLoader__IsEnabled(void);
void        PluginLoader__MenuCallback(void);
void        PluginLoader__UpdateMenu(void);
void        PluginLoader__HandleKernelEvent(u32 notifId);
void        PluginLoader__HandleCommands(void *ctx);
