#include "include.h"

PUBLIC void system_api(int edi, int esi, int ebp, int esp, int ebx, int edx, int ecx, int eax)
{
	if (edx == 1) { printstr((char *)ebx); }
	else if (edx == 2) { /* pass */ }
	else if (edx == 3) { /* pass */ }
	else if (edx == 4) { /* pass */ }
	else if (edx == 5) { /* pass */ }
	else if (edx == 6) { /* pass */ }
	else if (edx == 7) { /* pass */ }
	else if (edx == 8) { /* pass */ }
}
