// Implementation of cprintf console output for user processes,
// based on printfmt() and the write() system call.
//
// cprintf is a debugging statement, not a generic output statement.
// It is very important that it always go to the console, especially when
// debugging file descriptor code!

#include <parlib/common.h>
#include <parlib/parlib.h>
#include <stdio.h>
#include <unistd.h>
#include <parlib/spinlock.h>

// Collect up to BUF_SIZE characters into a buffer
// and perform ONE system call to print all of them,
// in order to make the lines output to the console atomic
// and prevent interrupts from causing context switches
// in the middle of a console output line and such.
#define BUF_SIZE 256
typedef struct debugbuf {
	size_t  idx;	// current buffer index
	size_t  cnt;	// total bytes printed so far
	uint8_t buf[BUF_SIZE];
} debugbuf_t;


static void putch(int ch, debugbuf_t **b)
{
	(*b)->buf[(*b)->idx++] = ch;
	if ((*b)->idx == BUF_SIZE) {
		write(1, (*b)->buf, (*b)->idx);
		(*b)->idx = 0;
	}
	(*b)->cnt++;
}

int akaros_vprintf(const char *fmt, va_list ap)
{
	debugbuf_t b;
	debugbuf_t *bp = &b;

	b.idx = 0;
	b.cnt = 0;
	akaros_vprintfmt((void*)putch, (void*)&bp, fmt, ap);
	write(1, b.buf, b.idx);

	return b.cnt;
}

int akaros_printf(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	if (in_vcore_context())
		ret = akaros_vprintf(format, ap);
	else
		ret = vprintf(format, ap);
	va_end(ap);
	return ret;
}

/* Poor man's Ftrace, won't work well with concurrency. */
static const char *blacklist[] = {
	"whatever",
};

static bool is_blacklisted(const char *s)
{
	#define ARRAY_SIZE(x) (sizeof((x))/sizeof((x)[0]))
	for (int i = 0; i < ARRAY_SIZE(blacklist); i++) {
		if (!strcmp(blacklist[i], s))
			return TRUE;
	}
	return FALSE;
}

static int tab_depth = 0;
static bool print = TRUE;

void reset_print_func_depth(void)
{
	tab_depth = 0;
}

void toggle_print_func(void)
{
	print = !print;
	printf("Func entry/exit printing is now %sabled\n", print ? "en" : "dis");
}

static spinlock_t lock = {0};

void __print_func_entry(const char *func, const char *file)
{
	if (!print)
		return;
	if (is_blacklisted(func))
		return;
	spinlock_lock(&lock);
	printd("Vcore %2d", vcore_id());	/* helps with multicore output */
	for (int i = 0; i < tab_depth; i++)
		printf("\t");
	printf("%s() in %s\n", func, file);
	spinlock_unlock(&lock);
	tab_depth++;
}

void __print_func_exit(const char *func, const char *file)
{
	if (!print)
		return;
	if (is_blacklisted(func))
		return;
	tab_depth--;
	spinlock_lock(&lock);
	printd("Vcore %2d", vcore_id());
	for (int i = 0; i < tab_depth; i++)
		printf("\t");
	printf("---- %s()\n", func);
	spinlock_unlock(&lock);
}
