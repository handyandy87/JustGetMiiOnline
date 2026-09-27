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

// The logging macro the files under youtube/ call, by the path WiiULeanback keeps its own logging
// header at. Here it's this plugin's log, so Enable Debugger Logging decides whether any of it is
// written.

#include "../logger.h"

#define DEBUG_FUNCTION_LINE(FMT, ...) LOG(FMT, ##__VA_ARGS__)
