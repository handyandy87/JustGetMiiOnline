#pragma once

// Puts accounts.google.com on the shell's host whitelist, which is what stops the page reaching
// sign-in for itself. One entry is given up for it; the list keeps its length.

// Finds the entry and rewrites it. Call once, from the application start hook. Answers whether
// the name is on the list, which is also true when it was already done.
bool whitelist_allow_accounts();
