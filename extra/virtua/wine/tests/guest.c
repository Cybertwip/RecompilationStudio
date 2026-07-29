/* A guest that exercises exactly what the loader must get right. */
extern int host_provided_symbol(int);        /* an import -> must resolve  */
extern int host_missing_symbol(int);         /* an import -> must be named */

static int datum_a = 0x11111111;
static int datum_b = 0x22222222;

/* Pointers to statics: each needs an R_ARM_RELATIVE at load time. */
int *const table[] = { &datum_a, &datum_b };

int guest_entry(int x) { return host_provided_symbol(x) + *table[0] + *table[1]; }
int guest_uses_missing(int x) { return host_missing_symbol(x); }
