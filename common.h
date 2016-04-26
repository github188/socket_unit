#ifndef _common_h_
#define _common_h_

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#define  compare_and_swap(lock,old,set)		__sync_bool_compare_and_swap(lock,old,set)
#define  fetch_and_add(value,add)			__sync_fetch_and_add(value,add)
#define	 fetch_and_sub(value,sub)			__sync_fetch_and_sub(value,sub)	




#undef  DBG_ON
#undef  FILE_NAME
#define 	DBG_ON  	(0x01)
#define 	FILE_NAME 	"common:"
#define 	dbg_printf(fmt,arg...) \
	do{if(DBG_ON)fprintf(stderr,FILE_NAME"%s(line=%d)->"fmt,__FUNCTION__,__LINE__,##arg);}while(0)


#endif