#include "platform_fault.h"
int fault_encountered = 0;

void mbedtls_platform_fault( void )
{
    fault_encountered = 1;
}
