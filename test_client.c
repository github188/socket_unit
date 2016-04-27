#include <stdio.h>
#include <stdlib.h>
#include "common.h"


#undef  DBG_ON
#undef  FILE_NAME
#define 	DBG_ON  	(0x01)
#define 	FILE_NAME 	"main:"



int main(void)
{
	dbg_printf("this is a test!\n");
	

}
