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

# Add app sources as each feature lands. mDNS/HTTPD are added in later phases.
lib = env.BuildLibrary(
    join("$BUILD_DIR", "LwipApps"),
    apps,
    "-<*> +<mqtt/mqtt.c>",
)
env.Append(LIBS=[lib])
