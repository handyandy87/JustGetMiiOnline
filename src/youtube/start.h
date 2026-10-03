#pragma once

// Points the app at a start url of the plugin's own, which carries the settings the page needs in
// its query. Call once, from the application start hook, after image_locate. Answers the url the
// app will load, or null when it is left on its own.
const char *start_url_point();
