#pragma once
// The extended-memory layouts (SysConfig::extMemConf), apart from the bank
// pool itself (ExtBankPool, extMem.h, which inherits them), so sysConfig.h
// needs no main-only headers: the Sidecar app builds sysConfig.cpp too.
struct ExtMemLayout {
    enum ExtMemConfig {
        NONE = 0,
        RAMBO256,
        NATIVE_XE_COMPY192,
        XE,
        AUTO   // init()'s haltAvailable and nativeBanks pick the layout (extMemPlanAuto())
    };
};
