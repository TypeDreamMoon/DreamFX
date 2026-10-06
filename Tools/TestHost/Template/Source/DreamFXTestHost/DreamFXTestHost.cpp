// Copyright (c) 2026 TypeDreamMoon. All rights reserved.

#include "DreamFXTestHost.h"
#include "Modules/ModuleManager.h"

// The host project has no behaviour of its own, so anything a run observes comes from the plugin under test.
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, DreamFXTestHost, "DreamFXTestHost");
