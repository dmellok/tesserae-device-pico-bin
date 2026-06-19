#
# lwip_prepend.py (PRE script): make the project's include/lwipopts.h win.
#
# The picosdk framework's CYW43/lwIP build adds the bundled btstack lwIP "port"
# directory (which has its own lwipopts.h) to the include path. Without this,
# the framework compiles lwIP core against btstack's lwipopts (LWIP_DNS off,
# no MQTT/SNTP/mDNS options) instead of ours, which both drops features and
# risks a config mismatch between the core and our code. Prepending the project
# include dir here (before the framework appends btstack's) ensures our
# lwipopts.h is found first by every translation unit.
#
Import("env")
env.Prepend(CPPPATH=[env["PROJECT_INCLUDE_DIR"]])
