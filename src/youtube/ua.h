#pragma once

// Changes the app's User-Agent just enough that youtubei.googleapis.com answers it. Call once,
// from the application start hook, after image_locate. Answers whether the agent is the changed
// one now.
bool ua_unblock();
