#include "strcpy.S"
#ifdef CONFIG_ALPHA_EV6
#include "ev6-strcat.S"
#include "ev6-stxcpy.S"
#else
#include "strcat.S"
#include "stxcpy.S"
#endif
