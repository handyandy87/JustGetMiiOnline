/*  Copyright 2026 HandyAndy87

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// The YouTube Patcher, which gets the Wii U YouTube app starting again. youtube.cpp has what it
// does. Its four settings are read and put in the menu through youtube/settings.h, which
// youtube.cpp implements.

#include <cstddef>

// What runs when a title starts and when it ends. Both do nothing outside the YouTube app, and
// the start does nothing in it with YouTube Patcher off.
void youtube_start();
void youtube_end();

// The name to resolve in place of the one the app asked for: the revival host for
// www.youtube.com, and the console for the proxy's own name while it's listening. Anything else,
// and anything asked outside a run the patcher started, comes back unchanged.
const char *youtube_answer(const char *node);

// One line of the Debug page's readout of the last YouTube start this boot. False once there are
// no more lines, which is right away with the patcher off and nothing started.
bool youtube_report_line(size_t index, char *out, size_t size);
