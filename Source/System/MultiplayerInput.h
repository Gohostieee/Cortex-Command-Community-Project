#pragma once

#include <array>
#include <SDL3/SDL_scancode.h>

namespace RTE::MP {
// Reserve the upper input bits for GUI navigation. These use the same cumulative
// event counters and timeout as gameplay controls, without leaking host keys.
inline constexpr unsigned GUIKeyFirst = 48;
inline constexpr std::array<SDL_Scancode, 16> GUIKeys{
	SDL_SCANCODE_SPACE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_RETURN,
	SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
	SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,
	SDL_SCANCODE_HOME, SDL_SCANCODE_END, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN
};
}
