#include "strncpy.S"
#ifdef CONFIG_ALPHA_EV6
#include "ev6-strncat.S"
#include "ev6-stxncpy.S"
#else
#include "strncat.S"
#include "stxncpy.S"
#endif
