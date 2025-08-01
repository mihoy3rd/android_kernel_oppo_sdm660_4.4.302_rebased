/*
 * This module exports the functions:
 *
 *     'int set_selection(struct tiocl_selection __user *, struct tty_struct *)'
 *     'void clear_selection(void)'
 *     'int paste_selection(struct tty_struct *)'
 *     'int sel_loadlut(char __user *)'
 *
 * Now that /dev/vcs exists, most of this can disappear again.
 */

#include <linux/module.h>
#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <asm/uaccess.h>

#include <linux/kbd_kern.h>
#include <linux/vt_kern.h>
#include <linux/consolemap.h>
#include <linux/selection.h>
#include <linux/tiocl.h>
#include <linux/console.h>
#include <linux/tty_flip.h>

/* Don't take this from <ctype.h>: 011-015 on the screen aren't spaces */
#define isspace(c)	((c) == ' ')

extern void poke_blanked_console(void);

/* FIXME: all this needs locking */
/* Variables for selection control. */
/* Use a dynamic buffer, instead of static (Dec 1994) */
struct vc_data *sel_cons;		/* must not be deallocated */
static int use_unicode;
static volatile int sel_start = -1; 	/* cleared by clear_selection */
static int sel_end;
static int sel_buffer_lth;
static char *sel_buffer;

/* clear_selection, highlight and highlight_pointer can be called
   from interrupt (via scrollback/front) */

/* set reverse video on characters s-e of console with selection. */
static inline void highlight(const int s, const int e)
{
	invert_screen(sel_cons, s, e-s+2, 1);
}

/* use complementary color to show the pointer */
static inline void highlight_pointer(const int where)
{
	complement_pos(sel_cons, where);
}

static u16
sel_pos(int n)
{
	return inverse_translate(sel_cons, screen_glyph(sel_cons, n),
				use_unicode);
}

/**
 *	clear_selection		-	remove current selection
 *
 *	Remove the current selection highlight, if any from the console
 *	holding the selection. The caller must hold the console lock.
 */
void clear_selection(void)
{
	highlight_pointer(-1); /* hide the pointer */
	if (sel_start != -1) {
		highlight(sel_start, sel_end);
		sel_start = -1;
	}
}

bool vc_is_sel(struct vc_data *vc)
{
	return vc == sel_cons;
}

/*
 * User settable table: what characters are to be considered alphabetic?
 * 256 bits. Locked by the console lock.
 */
static u32 inwordLut[8]={
  0x00000000, /* control chars     */
  0x03FF0000, /* digits            */
  0x87FFFFFE, /* uppercase and '_' */
  0x07FFFFFE, /* lowercase         */
  0x00000000,
  0x00000000,
  0xFF7FFFFF, /* latin-1 accented letters, not multiplication sign */
  0xFF7FFFFF  /* latin-1 accented letters, not division sign */
};

static inline int inword(const u16 c) {
	return c > 0xff || (( inwordLut[c>>5] >> (c & 0x1F) ) & 1);
}

/**
 *	set loadlut		-	load the LUT table
 *	@p: user table
 *
 *	Load the LUT table from user space. The caller must hold the console
 *	lock. Make a temporary copy so a partial update doesn't make a mess.
 */
int sel_loadlut(char __user *p)
{
	u32 tmplut[8];
	if (copy_from_user(tmplut, (u32 __user *)(p+4), 32))
		return -EFAULT;
	memcpy(inwordLut, tmplut, 32);
	return 0;
}

/* does screen address p correspond to character at LH/RH edge of screen? */
static inline int atedge(const int p, int size_row)
{
	return (!(p % size_row)	|| !((p + 2) % size_row));
}

/* constrain v such that v <= u */
static inline unsigned short limit(const unsigned short v, const unsigned short u)
{
	return (v > u) ? u : v;
}

/* stores the char in UTF8 and returns the number of bytes used (1-3) */
static int store_utf8(u16 c, char *p)
{
	if (c < 0x80) {
		/*  0******* */
		p[0] = c;
		return 1;
	} else if (c < 0x800) {
		/* 110***** 10****** */
		p[0] = 0xc0 | (c >> 6);
		p[1] = 0x80 | (c & 0x3f);
		return 2;
    	} else {
		/* 1110**** 10****** 10****** */
		p[0] = 0xe0 | (c >> 12);
		p[1] = 0x80 | ((c >> 6) & 0x3f);
		p[2] = 0x80 | (c & 0x3f);
		return 3;
    	}
}

/**
 *	set_selection		- 	set the current selection.
 *	@sel: user selection info
 *	@tty: the console tty
 *
 *	Invoked by the ioctl handle for the vt layer.
 *
 *	The entire selection process is managed under the console_lock. It's
 *	 a lot under the lock but its hardly a performance path
 */
static int __set_selection(const struct tiocl_selection __user *sel, struct tty_struct *tty)
{
	struct vc_data *vc = vc_cons[fg_console].d;
	int sel_mode, new_sel_start, new_sel_end, spc;
	char *bp, *obp;
	int i, ps, pe, multiplier;
	u16 c;
	int mode, ret = 0;

	poke_blanked_console();

	{ unsigned short xs, ys, xe, ye;

	  if (!access_ok(VERIFY_READ, sel, sizeof(*sel)))
		return -EFAULT;
	  __get_user(xs, &sel->xs);
	  __get_user(ys, &sel->ys);
	  __get_user(xe, &sel->xe);
	  __get_user(ye, &sel->ye);
	  __get_user(sel_mode, &sel->sel_mode);
	  xs--; ys--; xe--; ye--;
	  xs = limit(xs, vc->vc_cols - 1);
	  ys = limit(ys, vc->vc_rows - 1);
	  xe = limit(xe, vc->vc_cols - 1);
	  ye = limit(ye, vc->vc_rows - 1);
	  ps = ys * vc->vc_size_row + (xs << 1);
	  pe = ye * vc->vc_size_row + (xe << 1);

	  if (sel_mode == TIOCL_SELCLEAR) {
	      /* useful for screendump without selection highlights */
	      clear_selection();
	      return 0;
	  }

	  if (mouse_reporting() && (sel_mode & TIOCL_SELMOUSEREPORT)) {
	      mouse_report(tty, sel_mode & TIOCL_SELBUTTONMASK, xs, ys);
	      return 0;
	  }
        }

	if (ps > pe)	/* make sel_start <= sel_end */
	{
		int tmp = ps;
		ps = pe;
		pe = tmp;
	}

	if (sel_cons != vc_cons[fg_console].d) {
		clear_selection();
		sel_cons = vc_cons[fg_console].d;
	}
	mode = vt_do_kdgkbmode(fg_console);
	if (mode == K_UNICODE)
		use_unicode = 1;
	else
		use_unicode = 0;

	switch (sel_mode)
	{
		case TIOCL_SELCHAR:	/* character-by-character selection */
			new_sel_start = ps;
			new_sel_end = pe;
			break;
		case TIOCL_SELWORD:	/* word-by-word selection */
			spc = isspace(sel_pos(ps));
			for (new_sel_start = ps; ; ps -= 2)
			{
				if ((spc && !isspace(sel_pos(ps))) ||
				    (!spc && !inword(sel_pos(ps))))
					break;
				new_sel_start = ps;
				if (!(ps % vc->vc_size_row))
					break;
			}
			spc = isspace(sel_pos(pe));
			for (new_sel_end = pe; ; pe += 2)
			{
				if ((spc && !isspace(sel_pos(pe))) ||
				    (!spc && !inword(sel_pos(pe))))
					break;
				new_sel_end = pe;
				if (!((pe + 2) % vc->vc_size_row))
					break;
			}
			break;
		case TIOCL_SELLINE:	/* line-by-line selection */
			new_sel_start = ps - ps % vc->vc_size_row;
			new_sel_end = pe + vc->vc_size_row
				    - pe % vc->vc_size_row - 2;
			break;
		case TIOCL_SELPOINTER:
			highlight_pointer(pe);
			return 0;
		default:
			return -EINVAL;
	}

	/* remove the pointer */
	highlight_pointer(-1);

	/* select to end of line if on trailing space */
	if (new_sel_end > new_sel_start &&
		!atedge(new_sel_end, vc->vc_size_row) &&
		isspace(sel_pos(new_sel_end))) {
		for (pe = new_sel_end + 2; ; pe += 2)
			if (!isspace(sel_pos(pe)) ||
			    atedge(pe, vc->vc_size_row))
				break;
		if (isspace(sel_pos(pe)))
			new_sel_end = pe;
	}
	if (sel_start == -1)	/* no current selection */
		highlight(new_sel_start, new_sel_end);
	else if (new_sel_start == sel_start)
	{
		if (new_sel_end == sel_end)	/* no action required */
			return 0;
		else if (new_sel_end > sel_end)	/* extend to right */
			highlight(sel_end + 2, new_sel_end);
		else				/* contract from right */
			highlight(new_sel_end + 2, sel_end);
	}
	else if (new_sel_end == sel_end)
	{
		if (new_sel_start < sel_start)	/* extend to left */
			highlight(new_sel_start, sel_start - 2);
		else				/* contract from left */
			highlight(sel_start, new_sel_start - 2);
	}
	else	/* some other case; start selection from scratch */
	{
		clear_selection();
		highlight(new_sel_start, new_sel_end);
	}
	sel_start = new_sel_start;
	sel_end = new_sel_end;

	/* Allocate a new buffer before freeing the old one ... */
	multiplier = use_unicode ? 3 : 1;  /* chars can take up to 3 bytes */
	bp = kmalloc(((sel_end-sel_start)/2+1)*multiplier, GFP_KERNEL);
	if (!bp) {
		printk(KERN_WARNING "selection: kmalloc() failed\n");
		clear_selection();
		return -ENOMEM;
	}
	kfree(sel_buffer);
	sel_buffer = bp;

	obp = bp;
	for (i = sel_start; i <= sel_end; i += 2) {
		c = sel_pos(i);
		if (use_unicode)
			bp += store_utf8(c, bp);
		else
			*bp++ = c;
		if (!isspace(c))
			obp = bp;
		if (! ((i + 2) % vc->vc_size_row)) {
			/* strip trailing blanks from line and add newline,
			   unless non-space at end of line. */
			if (obp != bp) {
				bp = obp;
				*bp++ = '\r';
			}
			obp = bp;
		}
	}
	sel_buffer_lth = bp - sel_buffer;

	return ret;
}

int set_selection(const struct tiocl_selection __user *v, struct tty_struct *tty)
{
	int ret;

	mutex_lock(&sel_lock);
	console_lock();
	ret = __set_selection(v, tty);
	console_unlock();
	mutex_unlock(&sel_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(set_selection_kernel);

/* Insert the contents of the selection buffer into the
 * queue of the tty associated with the current console.
 * Invoked by ioctl().
 *
 * Locking: called without locks. Calls the ldisc wrongly with
 * unsafe methods,
 */
int paste_selection(struct tty_struct *tty)
{
	struct vc_data *vc = tty->driver_data;
	int	pasted = 0;
	unsigned int count;
	struct  tty_ldisc *ld;
	DECLARE_WAITQUEUE(wait, current);

	console_lock();
	poke_blanked_console();
	console_unlock();

	ld = tty_ldisc_ref_wait(tty);
	tty_buffer_lock_exclusive(&vc->port);

	add_wait_queue(&vc->paste_wait, &wait);
	while (sel_buffer && sel_buffer_lth > pasted) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (test_bit(TTY_THROTTLED, &tty->flags)) {
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		count = sel_buffer_lth - pasted;
		count = tty_ldisc_receive_buf(ld, sel_buffer + pasted, NULL,
					      count);
		pasted += count;
	}
	remove_wait_queue(&vc->paste_wait, &wait);
	__set_current_state(TASK_RUNNING);

	tty_buffer_unlock_exclusive(&vc->port);
	tty_ldisc_deref(ld);
	return 0;
}
