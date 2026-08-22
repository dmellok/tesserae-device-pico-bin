#
# lwip_apps.py: compile the lwIP application sources we use into a library and
# link it.
#
# The picosdk framework builds lwIP's core (api/core/port/netif) but not the
# protocol "apps" (MQTT, SNTP, mDNS, HTTPD). This post-script builds the ones
# this firmware needs from the framework's bundled lwIP as a static library, so
# we do not have to vendor copies into the repo. App options live in
# include/lwipopts.h.
#
Import("env")
from os.path import join

platform = env.PioPlatform()
fw = platform.get_package_dir("framework-picosdk")
apps = join(fw, "lib", "lwip", "src", "apps")

# lwIP's http_client app is deliberately NOT here: it cannot set request headers
# (no Authorization, no If-None-Match), which the Tesserae protocol needs on
# every call. src/net_httpc.c replaces it with a client written directly against
# altcp, which also gets TLS for free. See net_httpc.h.
lib = env.BuildLibrary(
    join("$BUILD_DIR", "LwipApps"),
    apps,
    "-<*> +<mqtt/mqtt.c> +<sntp/sntp.c> "
    "+<mdns/mdns.c> +<mdns/mdns_domain.c> +<mdns/mdns_out.c>",
)
env.Append(LIBS=[lib])
