// asset_test_stubs.c — C stubs for asset unit tests.
// provides __um_sites/__um_sites_count for mem.c.

typedef struct { const char *file; unsigned line; unsigned col; } um_site_info_t;
const um_site_info_t __um_sites[]  = {};
const unsigned int   __um_sites_count = 0;

int rt_log_level = 3;
