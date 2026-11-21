// selftest_port.h - this port's additions to the shared selftest.c (3ds-test-suite hwtest/selftest).
// selftest.c includes this file twice: ST_PORT_CONFIG (settings) and ST_PORT_CODE (extra code).
#ifdef ST_PORT_CODE
// Always-on crash restart (3ds-test-suite hwtest/restartguard): compiled into the selftest object
// so it is always linked.
#include "restartguard.c"
#endif
