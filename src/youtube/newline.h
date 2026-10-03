#pragma once

// Lets the page tell the GamePad keyboard's Return key from its OK button. The shell sends both
// as the Enter key, so this has a typed newline arrive as a newline character instead, and OK
// stays as it was.

// Finds the site and patches it. Call once, from the application start hook. Answers whether a
// typed newline now reaches the page as one, which is also true when it was already patched.
bool newline_apart();
