//
// Joystick support, replaced by nothing.
//
// Upstream's i_joystick.c is SDL: it opens an SDL_Joystick and polls it. There
// is no joystick on this machine and no SDL to read one with, so the four
// entry points the rest of doom calls are stubbed out here rather than the
// calls being removed from d_main.c and i_system.c - keeping the callers
// unmodified is what makes the vendored sources diffable against upstream.
//
// I_UpdateJoystick is called once per tic from the input loop and
// I_ShutdownJoystick from the exit chain; both are no-ops for the same reason.

#include "doomtype.h"
#include "i_joystick.h"

void I_InitJoystick(void)
{
}

void I_ShutdownJoystick(void)
{
}

void I_UpdateJoystick(void)
{
}

void I_BindJoystickVariables(void)
{
}
