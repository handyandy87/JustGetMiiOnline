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

// Share to Miiverse from the YouTube app: a video's title and link, posted through the
// console's own post screen to the Miiverse selected in the menu.
//
// YouTube doesn't import the Miiverse library, so this loads it into the app's process,
// sets it up once and opens the post screen on a thread of its own. Loading it is a late
// load, so olv.cpp's watch points it at the selected Miiverse as it comes in, and this
// checks where it points before anything is posted through it. olv_share.cpp has the steps.

#include <cstddef>

// Starts a share and answers the HTTP status the page gets back: 202 when the thread that
// opens the post screen is running, 409 while one is or within a minute of the last one
// that reached the post screen, 503 when the selected Miiverse can't be posted to from
// this process, and 400 for a bad video id or a title or channel that isn't text. Returns
// right away; the slow part is the thread's.
int olv_share_start(const char *video_id, const char *title, const char *channel);

// Whether the selected Miiverse is one a share can post to, which is also whether the page
// shows the button. False for the rest of the process once a share found the library
// pointing anywhere else.
bool olv_share_available();

// Where a share is, for the page's spinner, as youtube/share.h's SHARE_STATE_* values: idle with
// nothing running, opening while the thread is still loading the library or asking for the post
// screen, and open once the post screen was asked for and the app hasn't had the foreground back
// yet. It drops to idle once what came of it is read, or as soon as a step fails.
int olv_share_state();

// Per process, from the application start hook. Everything a share sets up lives in the
// process it ran in, so the next title starts from nothing. The Debug page's readout is
// kept until the next share.
void olv_share_reset();

// Loads nn_olv on the app's own thread at application start, when a share could happen, so the
// share thread finds it loaded. Call after olv_share_reset and after the late-load watch is set.
void olv_share_preload();

// From the foreground hook. Reads what came of the post once the post screen hands the app
// back, and does nothing at any other time.
void olv_share_foreground();

// One line of the Debug page's readout of the last share this boot. False once there are
// no more, which is right away until one has run.
bool olv_share_report_line(size_t index, char *out, size_t size);
