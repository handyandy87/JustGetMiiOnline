#pragma once

// Lets the app offer TLS 1.2. Its own OpenSSL can already do it and Chromium's net layer turns
// it off, so this clears that one flag and nothing else.

// Finds the site and patches it. Call once, from the application start hook. Answers whether the
// app will offer TLS 1.2, which is also true when it was already patched.
bool tls12_enable();
