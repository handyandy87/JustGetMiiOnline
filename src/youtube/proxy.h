#pragma once

// A small forwarder the page can reach, for the second screen lounge, which Google will not let
// it ask for itself. Listens on the console, adds the cross origin headers the answer is missing,
// and carries the request on to youtube over TLS.

// The name the page asks for and the port it asks on. The name resolves to the console because
// getaddrinfo answers it, and it is a youtube.com one so the shell's host whitelist covers it.
#define PROXY_HOST "lb-proxy.youtube.com"
#define PROXY_PORT 8099

// What the page asks for to find out whether the listener is there. Answered here and never
// carried upstream, so it costs one round trip on the console and nothing to youtube.
#define PROXY_PROBE_PATH "/__leanback_proxy"

// Starts the listener. Call once, from the application start hook. Answers whether it is up.
bool proxy_start();

// Whether the listener is actually up. Answering the name locally when it is not leaves the page
// with a refused connection on a name that resolved, instead of the lookup failure it knows how
// to fall back from.
bool proxy_running();

// Stops it and waits for the accept loop to notice.
void proxy_stop();

// A name lookup that does not go through the plugin's own hook. Asking for www.youtube.com
// through that would answer with the revival host, and the proxy would forward to it in a circle.
struct addrinfo;
int proxy_resolve(const char *node, const char *service, const struct addrinfo *hints,
                  struct addrinfo **res);
