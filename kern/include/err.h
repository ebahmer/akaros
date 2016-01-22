/* Plan9 style error popping.  For details, read Documentation/plan9.txt */

#pragma once

#include <setjmp.h>
#include <kthread.h>
#include <error.h>

#define ERRSTACK(x) struct errbuf *prev_errbuf; struct errbuf errstack[(x)];   \
                    int curindex = 0;
#define waserror() setjmp(&(errpush(errstack, ARRAY_SIZE(errstack), &curindex, \
									&prev_errbuf)->jmpbuf))
#define error(e, x, ...)												\
	do {																\
		set_error(e, x, ##__VA_ARGS__);									\
		longjmp(&get_cur_errbuf()->jmpbuf, 1);							\
	} while(0)
#define nexterror() longjmp(&(errpop(errstack, ARRAY_SIZE(errstack), &curindex, \
									 prev_errbuf)->jmpbuf), 1)
#define poperror() errpop(errstack, ARRAY_SIZE(errstack), &curindex,	\
						  prev_errbuf)
/* Give Akaros people a hint that we need a real error message.
 * The unreadability of this define is thanks to the 1890 US census.
 * Wrap at 80 or die! */

#define ERROR_FIXME "This space in " __FILE__ "@" \
	__STRINGIFY(__LINE__) " needs filling in."

struct errbuf *errpush(struct errbuf *errstack, int stacksize, int *curindex,
						struct errbuf **prev_errbuf);
struct errbuf *errpop(struct errbuf *errstack, int stacksize, int *curindex,
					  struct errbuf *prev_errbuf);
