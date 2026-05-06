#pragma once

struct DashData {
    float speedMph    = 0;
    float batV        = 24.6f;
    float rpm         = 0;
    float amps        = 0;
    char  mode[8]     = "NORMAL";
    char  state[8]    = "IDLE";
    float setpointPct = 0;
    float livePct     = 0;
    float rampPct     = 0;
};
