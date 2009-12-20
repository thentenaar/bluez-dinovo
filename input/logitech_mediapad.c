/************************ Logitech Mediapad Driver ********************************
 *     (C) 2006-2009 Tim Hentenaar <tim@hentenaar.com>                            *
 *     Licensed under the GNU General Public License (v2).                        *
 *     For more information, see http://hentenaar.com                             *
 *                                                                                *
 *     12/19/09 thentenaar:                                                       *
 *               * Added new DBus methods:                                        *
 *                   * GetKeyBindings                                             *
 *                   * WriteRawData                                               *
 *                   * SetInputMode                                               *
 *                                                                                *
 *               * Added support for setting the input mode selector,             *
 *                 and mode switch notification.                                  *
 *               * The Numlock toggle command is now ignored.                     *
 *                                                                                *
 *     12/17/09 thentenaar:                                                       *
 *               * Made simple DBus methods more generic.                         *
 *               * Added new DBus method: SetDisplayMode                          *
 *                                                                                *
 *     12/16/09 thentenaar:                                                       *
 *               * Added new DBus methods: BindKey, SetScreenMode                 *
 *               * Added keymap tables, updated default keysyms.                  *
 *               * Massively cleaned up the code.                                 *
 *               * Integrated atomic ops from Glen Rolle's 3.36 patch.            *
 *               * Rewrote DBus code.                                             *
 *               * Ported mediapad driver up to master.                           *
 *               * Forked bluez git.                                              *
 *                                                                                *
 **********************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <glib.h>
#include <syslog.h>
#include <gdbus.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fakehid.h"
#include "uinput.h"
#include "logging.h"

/* Screen modes */
#define LCD_SCREEN_MODE_TEXT   0x00
#define LCD_SCREEN_MODE_CLOCK  0x01

/* Display modes */
#define LCD_DISP_MODE_INIT    0x01 /* Initialize the line */
#define LCD_DISP_MODE_BUF1    0x10 /* Display the first buffer on the line */
#define LCD_DISP_MODE_BUF2    0x11 /* ... 2nd buffer */
#define LCD_DISP_MODE_BUF3    0x12 /* ... 3rd buffer */
#define LCD_DISP_MODE_SCROLL  0x20 /* Scroll by one buffer */
#define LCD_DISP_MODE_SCROLL2 0x02 /* ... by 2 buffers */
#define LCD_DISP_MODE_SCROLL3 0x03 /* ... by 3 buffers */

/* Icons */
#define LCD_ICON_EMAIL  0x01
#define LCD_ICON_IM     0x02
#define LCD_ICON_MUTE   0x04
#define LCD_ICON_ALERT  0x08
#define LCD_ICON_ALL    0x0f

/* Icon states */
#define LCD_ICON_OFF    0x00
#define LCD_ICON_ON     0x01
#define LCD_ICON_BLINK  0x02

/* Speaker / LED */
#define LCD_LOW_BEEP	0x01
#define LCD_LONG_BEEP	0x02
#define LCD_SHORT_BEEP	0x03
#define LCD_LED_ON      0x01
#define LCD_LED_OFF     0x02

/* DBus Paths */
#define MP_DBUS_INTF	"com.hentenaar.Dinovo.MediaPad"
#define MP_DBUS_PATH	"/com/hentenaar/Dinovo/MediaPad"

/* Lengths */
#define LCD_BUF_LEN     16
#define LCD_LINE_LEN    (LCD_BUF_LEN*3)

/* Media key scancodes */
#define MP_KEY_MEDIA    0x83
#define MP_KEY_FFWD     0xb5
#define MP_KEY_REW      0xb6
#define MP_KEY_STOP     0xb7
#define MP_KEY_PLAY     0xcd
#define MP_KEY_MUTE     0xe2
#define MP_KEY_VOLUP    0xe9
#define MP_KEY_VOLDOWN  0xea

/* Media pad input mode constants */
#define MP_INPUT_MODE_CALC 0x0b
#define MP_INPUT_MODE_NAV  0x0c
#define MP_INPUT_MODE_NUM  0x0d

/* This is easier than including device.h, etc. */
struct fake_input {
	int		flags;
	GIOChannel	*io;
	int		uinput;		/* uinput socket */
	int		rfcomm;		/* RFCOMM socket */
	uint8_t		ch;		/* RFCOMM channel number */
	gpointer connect;
	gpointer disconnect;
	void		*priv;
};

/* Mediapad State */
struct mp_state {
	int mode;
	int discard_keyup;
	int prev_key;
	int icons;
	int uinput;
	int sock;
	DBusConnection *db_conn;
};

/* Mediapad Command */
struct mpcmd {	
	char    command[22];
	uint8_t len;
};

struct mpcmd screen_mode = { /* 0 = text, 1 =  clock */
	{ 0xA2, 0x10, 0x00, 0x80, 0x10, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd screen_start = { /* Signals the start of a screen write operation (mode) */
	{ 0xA2, 0x10, 0x00, 0x81, 0x10, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd screen_finish = { /* Signals the end of a screen write operation */
	{ 0xA2, 0x10, 0x00, 0x83, 0x11, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd display_mode = { /* Set the display mode of a line */
	{ 0xA2, 0x10, 0x00, 0x80, 0x12, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd input_mode = { /* Set the input mode selector */
	{ 0xA2, 0x01, 0x00 }, 3
};

struct mpcmd enable_mode_notification = { /* Enables mode switch notifications */
	{ 0xA2, 0x10, 0x00, 0x80, 0x00, 0x51, 0x00, 0x00 }, 8
};

static struct mpcmd set_icons = { /* Set Icons (0 = off) */
	{ 0xA2, 0x11, 0x00, 0x82, 0x11, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 21
};

static struct mpcmd set_text_buffer = { /* Write a single buffer to the LCD */
	{ 0xA2, 0x11, 0x00, 0x82, 0x20, 0x20, 0x20, 0x20, 0x20, 
	  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 }, 21
};

static struct mpcmd set_ledspk[] = { /* LED / Speaker Control */
	{{ 0xA2, 0x10, 0x00, 0x81, 0x50, 0x00, 0x00, 0x00 }, 8},
	{{ 0xA2, 0x10, 0x00, 0x80, 0x50, 0x00, 0x00, 0x00 }, 8},
	{{ 0 }, 0}
};

static struct mpcmd setclk[] = { /* Set the clock */ 
	{{ 0xA2, 0x10, 0x00, 0x80, 0x31, 0x00, 0x00, 0x00 }, 8},
	{{ 0xA2, 0x10, 0x00, 0x80, 0x32, 0x02, 0x00, 0x00 }, 8},
	{{ 0xA2, 0x10, 0x00, 0x80, 0x33, 0x00, 0x00, 0x00 }, 8},
	{{ 0 }, 0}
};

/**
 * Mediapad Keymap (non-media)
 */
static uint8_t mp_keymap[2][16] = {
	/* Numeric mode */
	{ 
		KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER,
		KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
		KEY_7, KEY_8, KEY_9, KEY_0, KEY_DOT
	},

	/* Nav mode */
	{
		KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER,
		KEY_OPEN, KEY_LEFTMETA, KEY_UNDO, KEY_LEFT, KEY_DOWN, KEY_RIGHT,
		KEY_BACK, KEY_UP, KEY_FORWARD, KEY_0, KEY_DOT
	}
};

/**
 * Mediapad Keymap (media)
 */
static uint8_t mp_keymap_m[2][8] = {
	/* Numeric mode */
	{ 
		KEY_MEDIA, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_STOP,
		KEY_PLAYPAUSE, KEY_MUTE,  KEY_VOLUMEUP, KEY_VOLUMEDOWN
	},

	/* Nav mode */
	{
		KEY_MEDIA, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_STOP,
		KEY_PLAYPAUSE, KEY_MUTE,  KEY_VOLUMEUP, KEY_VOLUMEDOWN
	}
};

#define inject_key(X,Y,Z)         { send_event(X,EV_KEY,Y,Z); send_event(X,EV_SYN,SYN_REPORT,0); }
#define do_write(X,Y,Z)           if (write(X,Y,Z)) {};
#define mp_lcd_write_start(sock)   write_mpcmd(sock,screen_start)
#define mp_lcd_write_finish(sock)  write_mpcmd(sock,screen_finish)

/* Forward declarations to satisfy warnings... */
int logitech_mediapad_setup_uinput(struct fake_input *fake_input, struct fake_hid *fake_hid);
gboolean logitech_mediapad_event(GIOChannel *chan, GIOCondition cond, gpointer data);

/**
 * Send a uinput event
 */
static void send_event(int fd, uint16_t type, uint16_t code, int32_t value) {
	struct uinput_event event;

	memset(&event,0,sizeof(event));
	event.type	= type;
	event.code	= code;
	event.value	= value;
	gettimeofday(&event.time,NULL);
	do_write(fd,&event,sizeof(event));
}

/**
 * Translate a key scancode to a uinput key identifier 
 */
static uint8_t translate_key(int mode, int key) {
	/* Media keys */
	if ((key & 0xff) > 0x82) {
		switch (key & 0xff) {
			case MP_KEY_MEDIA:   return mp_keymap_m[mode ? 1 : 0][0];
			case MP_KEY_FFWD:    return mp_keymap_m[mode ? 1 : 0][1];
			case MP_KEY_REW:     return mp_keymap_m[mode ? 1 : 0][2];
			case MP_KEY_STOP:    return mp_keymap_m[mode ? 1 : 0][3];
			case MP_KEY_PLAY:    return mp_keymap_m[mode ? 1 : 0][4];
			case MP_KEY_MUTE:    return mp_keymap_m[mode ? 1 : 0][5];
			case MP_KEY_VOLUP:   return mp_keymap_m[mode ? 1 : 0][6];
			case MP_KEY_VOLDOWN: return mp_keymap_m[mode ? 1 : 0][7];
		}
	} 
	
	/* Non-media keys */
	if (key > 0x63) return KEY_UNKNOWN;
	return mp_keymap[mode ? 1 : 0][key-0x54];
}

/**
 * Write a command to the mediapad 
 */
static void write_mpcmd(int sock, struct mpcmd command) {
	if (sock < 4) return;
	do_write(sock,command.command,command.len);
}

/*
 * Set LCD mode
 */
static void mp_lcd_set_screen_mode(int sock, uint8_t mode) {
	screen_mode.command[6] = (char)mode;
	write_mpcmd(sock,screen_mode);
}

/**
 * Set the input mode selector 
 */
static void mp_set_input_mode(int sock, uint8_t mode) {
	input_mode.command[2] = (mode ? 0 : 1);
	write_mpcmd(sock,input_mode);
}

/**
 * Set display mode
 */
static void mp_lcd_set_display_mode(int sock,uint8_t mode1, uint8_t mode2, uint8_t mode3) {
	display_mode.command[5] = mode1;
	display_mode.command[6] = mode2;
	display_mode.command[7] = mode3;
	write_mpcmd(sock,display_mode);
}

/**
 * Set the status of one or more indicators
 */
static void mp_lcd_set_indicator(int sock, uint8_t indicator, uint8_t blink) {
	uint8_t mode = (blink >= 1) ? ((blink == 2) ? LCD_ICON_BLINK : LCD_ICON_ON) : 0; 
	uint8_t sel = 5;

	if (sock < 4 || indicator == 0) return;
	while (!(indicator & 1)) { sel++; indicator >>= 1; }
	while (indicator & 1) { set_icons.command[sel++] = mode; indicator >>= 1; }
	write_mpcmd(sock,set_icons);
}

/**
 * Clear the screen
 */
static void mp_lcd_clear(int sock) {
	mp_lcd_set_screen_mode(sock,LCD_SCREEN_MODE_CLOCK);
	mp_lcd_write_start(sock);
	mp_lcd_set_indicator(sock,LCD_ICON_ALL,LCD_ICON_OFF);
	mp_lcd_write_finish(sock);
}

/**
 * Manipulate the speaker / LED 
 */
static void mp_blink_or_beep(int sock, uint8_t beep, uint8_t blink) {
	int i = 0;

	set_ledspk[1].command[5] = 0; set_ledspk[1].command[6] = 0;
	if (beep)  set_ledspk[1].command[5] = (beep & 3);
	if (blink) set_ledspk[1].command[6] = 1;
	while (set_ledspk[i].len != 0) { write_mpcmd(sock,set_ledspk[i]); i++; }
}

/**
 * Set the Mediapad's clock
 */
static void mp_set_clock(int sock) {
	struct tm tx; time_t tim = 0; int i = 0;

	if (sock < 4) return;
	time(&tim); localtime_r(&tim,&tx);
	setclk[0].command[5] = (char)(tx.tm_sec);
	setclk[0].command[6] = (char)(tx.tm_min);
	setclk[0].command[7] = (char)(tx.tm_hour);
	setclk[1].command[6] = (char)(tx.tm_mday);
	setclk[1].command[7] = (char)(tx.tm_mon);
	setclk[2].command[5] = (char)(tx.tm_year - 100);
	
	while (setclk[i].len != 0) { write_mpcmd(sock,setclk[i]); i++; }
}

/**
 * Write a single buffer of text to the LCD (<= 16 chars.)
 */
static void mp_lcd_write_buffer(int sock, char *text, uint8_t bufno) {
	if (!text || sock < 4 || bufno > 9) return;
	set_text_buffer.command[4] = 0x20 + bufno;
	memcpy(&set_text_buffer.command[5],text,(strlen(text) > LCD_BUF_LEN) ? LCD_BUF_LEN : strlen(text));
	write_mpcmd(sock,set_text_buffer);
}

/**
 * Write a single line of text to the LCD (<= 48 chars.)
 */
static void mp_lcd_write_line(int sock, char *text, uint8_t lineno) {
	char line[LCD_LINE_LEN]; uint32_t i = 0,z = 0; uint8_t f = LCD_DISP_MODE_BUF1;

	if (!text || sock < 4) return;
	lineno = (lineno > 3) ? 3 : (!lineno) ? 1 : lineno;
	z      = (strlen(text) > LCD_LINE_LEN) ? LCD_LINE_LEN : strlen(text);

	/* Copy the line text */
	memset(line,0x20,LCD_LINE_LEN);
	memcpy(line,text,z);

	/* Adjust flags for autoscrolling */
	if (z > LCD_BUF_LEN) {
		f |= LCD_DISP_MODE_SCROLL | LCD_DISP_MODE_SCROLL2;
		if (z > LCD_BUF_LEN*2) f++;
	}

	/* Write the text */
	mp_lcd_write_start(sock);
	mp_lcd_set_display_mode(sock,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT);
	mp_lcd_set_screen_mode(sock,LCD_SCREEN_MODE_TEXT);
	for (i=0;i<3;i++) mp_lcd_write_buffer(sock,line+i*LCD_BUF_LEN,lineno*3+i);
	mp_lcd_set_display_mode(sock,f,f,f);
	mp_lcd_write_finish(sock);
}

/**
 * Write a buffer of text to the LCD -- with autoscrolling. (<= 144 chars)
 */
static void mp_lcd_write_text(int sock, char *text) {
	char lines[LCD_BUF_LEN*9]; uint32_t i = 0,z = 0; 
	uint8_t f1 = LCD_DISP_MODE_BUF1, f2 = LCD_DISP_MODE_BUF1, f3 = LCD_DISP_MODE_BUF1;

	if (!text || sock < 4) return;
	z = (strlen(text) > LCD_BUF_LEN*9) ? LCD_BUF_LEN*9 : strlen(text);

	/* Copy the text */
	memset(lines,0x20,LCD_BUF_LEN*9);
	memcpy(lines,text,z);

	/* Set flags for autoscrolling */
	if (z > LCD_BUF_LEN*3) { 
		f1 |= LCD_DISP_MODE_SCROLL | LCD_DISP_MODE_SCROLL2; 
		f2 = f3 = f1;
		if (z >= LCD_BUF_LEN*6) { f1++; f2++; f3++; }
	}

	/* Write the text */
	mp_lcd_write_start(sock);
	mp_lcd_set_display_mode(sock,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT);
	mp_lcd_set_screen_mode(sock,LCD_SCREEN_MODE_TEXT);
	for (i=0;i<3;i++) {
		mp_lcd_write_buffer(sock,lines+(LCD_BUF_LEN*(i*3)),i);
		mp_lcd_write_buffer(sock,lines+(LCD_BUF_LEN*(i*3+1)),i+3);
		mp_lcd_write_buffer(sock,lines+(LCD_BUF_LEN*(i*3+2)),i+6);
	}
	mp_lcd_set_display_mode(sock,f1,f2,f3);
	mp_lcd_write_finish(sock);
}	

/**************** DBus Methods *******************/
typedef DBusMessage *(*MPDBusMethodFunction)(DBusMessage *msg, struct mp_state *mp, void *data);

typedef struct {
	const char *name;
	const char *signature;
	const char *reply;
	MPDBusMethodFunction function;
	GDBusMethodFlags flags;
	void *proc;
} MPDBusMethodTable;

typedef void (*MPGenericProc)(int);
typedef void (*MPGenericProc1u)(int,uint32_t);
typedef void (*MPGenericProc2u)(int,uint32_t,uint32_t);
typedef void (*MPGenericProc3u)(int,uint32_t,uint32_t,uint32_t);

static DBusMessage *mp_dbus_generic_method(DBusMessage *msg, struct mp_state *mp, void *proc) {
	if (!mp || !proc) return NULL;
	((MPGenericProc)proc)(mp->sock);
	return NULL;
}

static DBusMessage *mp_dbus_generic_1u_method(DBusMessage *msg, struct mp_state *mp, void *proc) {
	DBusError db_err; uint32_t u1;

	if (!mp || !proc) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&u1,DBUS_TYPE_INVALID);
	if (!dbus_error_is_set(&db_err)) ((MPGenericProc1u)(proc))(mp->sock,u1);
	dbus_error_free(&db_err);
	return NULL;
}

static DBusMessage *mp_dbus_generic_2u_method(DBusMessage *msg, struct mp_state *mp, void *proc) {
	DBusError db_err; uint32_t u1,u2;

	if (!mp || !proc) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&u1,DBUS_TYPE_UINT32,&u2,DBUS_TYPE_INVALID);
	if (!dbus_error_is_set(&db_err)) ((MPGenericProc2u)(proc))(mp->sock,u1,u2);
	dbus_error_free(&db_err);
	return NULL;
}

static DBusMessage *mp_dbus_generic_3u_method(DBusMessage *msg, struct mp_state *mp, void *proc) {
	DBusError db_err; uint32_t u1,u2,u3;

	if (!mp || !proc) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&u1,DBUS_TYPE_UINT32,&u2,DBUS_TYPE_UINT32,&u3,DBUS_TYPE_INVALID);
	if (!dbus_error_is_set(&db_err)) ((MPGenericProc3u)(proc))(mp->sock,u1,u2,u3);
	dbus_error_free(&db_err);
	return NULL;
}

/* BindKey(scancode,mode,key)  - see <linux/input.h> for KEY_* values
 *	[ scancode := Mediapad scancode ]
 *	[ mode     := 0 (normal) | 1 (nav) ]
 *	[ key      := key value to translate to (e.g. KEY_*) ]
 */ 
static DBusMessage *mp_dbus_bind_key(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusError db_err; uint32_t scancode,mode,key;

	if (!mp) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&scancode,DBUS_TYPE_UINT32,&mode,DBUS_TYPE_UINT32,&key,DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&db_err)) error("logitech_mediapad: BindKey: unable to get args! (%s)",db_err.message);
	else {
		/* Media keys */
		if (scancode > 0x82) {
			switch (scancode) {
				case MP_KEY_MEDIA:   mp_keymap_m[mode ? 1 : 0][0] = key; break;
				case MP_KEY_FFWD:    mp_keymap_m[mode ? 1 : 0][1] = key; break;
				case MP_KEY_REW:     mp_keymap_m[mode ? 1 : 0][2] = key; break;
				case MP_KEY_STOP:    mp_keymap_m[mode ? 1 : 0][3] = key; break;
				case MP_KEY_PLAY:    mp_keymap_m[mode ? 1 : 0][4] = key; break;
				case MP_KEY_MUTE:    mp_keymap_m[mode ? 1 : 0][5] = key; break;
				case MP_KEY_VOLUP:   mp_keymap_m[mode ? 1 : 0][6] = key; break;
				case MP_KEY_VOLDOWN: mp_keymap_m[mode ? 1 : 0][7] = key; break;
			}
		} 
	
		/* Non-media keys */
		if (scancode < 0x63) mp_keymap[mode ? 1 : 0][scancode-0x54] = key;
	}

	dbus_error_free(&db_err);
	return NULL;
}

/* WriteText(text) Max Length: 144 */
static DBusMessage *mp_dbus_write_text(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args; char *text;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&db_args,&text);
			if (text && strlen(text) > 0) mp_lcd_write_text(mp->sock,text);
		}
	}

	return NULL;
}

/* WriteLine(lineno, text) Max Length: 48 */
static DBusMessage *mp_dbus_write_line(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args; char *text; uint32_t lineno;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&lineno);
		if (dbus_message_iter_has_next(&db_args)) {
			dbus_message_iter_next(&db_args);
			if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&db_args,&text);
				if (text && strlen(text) > 0) mp_lcd_write_line(mp->sock,text,lineno);
			}
		}
	}

	return NULL;
}

/* WriteBuffer(bufno, text) Max Length: 16 */
static DBusMessage *mp_dbus_write_buffer(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args; char *text; uint32_t bufno;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&bufno);
		if (dbus_message_iter_has_next(&db_args)) {
			dbus_message_iter_next(&db_args);
			if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&db_args,&text);
				if (text && strlen(text) > 0) mp_lcd_write_buffer(mp->sock,text,bufno);
			}
		}
	}

	return NULL;
}

/* WriteTextBin(chars) Max Length: 144 */
static DBusMessage *mp_dbus_write_text_bin(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args,db_sub; uint32_t val,i; char *chars;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&db_args,&db_sub);
			if ((chars = g_new0(char,1+(16*9)))) {
				for (i=0;i<=16*9;i++) {
					dbus_message_iter_get_basic(&db_sub,&val);
					chars[i] = (char)val;
					if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
					else break;
				} 

				if (i > 0) mp_lcd_write_text(mp->sock,chars); 
				g_free(chars);
			}
		}
	}

	return NULL;
}

/* WriteLineBin(lineno, chars) Max Length: 48 */
static DBusMessage *mp_dbus_write_line_bin(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args,db_sub; char *chars; uint32_t lineno,val,i;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&lineno);
			if (dbus_message_iter_has_next(&db_args)) {
				dbus_message_iter_next(&db_args);
				if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
					dbus_message_iter_recurse(&db_args,&db_sub);
					if ((chars = g_new0(char,1+(16*3)))) {
						for (i=0;i<=16*3;i++) {
							dbus_message_iter_get_basic(&db_sub,&val);
							chars[i] = (char)val;
							if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
							else break;
						} 
						if (i > 0) mp_lcd_write_line(mp->sock,chars,lineno); 
						g_free(chars);
					}
				}
			}
	}

	return NULL;
}

/* WriteBufferBin(lineno, chars) Max Length: 16 */
static DBusMessage *mp_dbus_write_buffer_bin(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args,db_sub; char *chars; uint32_t bufno,val,i;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&bufno);
			if (dbus_message_iter_has_next(&db_args)) {
				dbus_message_iter_next(&db_args);
				if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
					dbus_message_iter_recurse(&db_args,&db_sub);
					if ((chars = g_new0(char,1+(16*3)))) {
						for (i=0;i<=16;i++) {
							dbus_message_iter_get_basic(&db_sub,&val);
							chars[i] = (char)val;
							if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
							else break;
						} 
						if (i > 0) mp_lcd_write_buffer(mp->sock,chars,bufno); 
						g_free(chars);
					}
				}
			}
	}

	return NULL;
}

/* GetKeyBindings() */
static DBusMessage *mp_dbus_get_key_bindings(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessage *ret; uint8_t *ptr1,*ptr2,*ptr3,*ptr4;

	if (!mp) return NULL;
	if (!(ret = dbus_message_new_method_return(msg))) return NULL;

	ptr1 = mp_keymap[0];   ptr2 = mp_keymap[1];
	ptr3 = mp_keymap_m[0]; ptr4 = mp_keymap_m[1];
	dbus_message_append_args(ret,
		DBUS_TYPE_ARRAY,DBUS_TYPE_BYTE,&ptr1,16, /* Num mode keys */
		DBUS_TYPE_ARRAY,DBUS_TYPE_BYTE,&ptr2,16, /* Nav mode keys */
		DBUS_TYPE_ARRAY,DBUS_TYPE_BYTE,&ptr3,8,  /* Num mode media keys */
		DBUS_TYPE_ARRAY,DBUS_TYPE_BYTE,&ptr4,8,  /* Nav mode media keys */
		DBUS_TYPE_INVALID);

	return ret;
}

/* WriteRawData(data) */
static DBusMessage *mp_dbus_write_raw_data(DBusMessage *msg, struct mp_state *mp, void *data) {
	DBusMessageIter db_args,db_sub; uint32_t val,len=0; char *chars=NULL;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&db_args,&db_sub);
			chars = g_new0(char,1);
			while (1) {
				if ((chars = g_realloc(chars,sizeof(char)*(++len)))) {
					dbus_message_iter_get_basic(&db_sub,&val);
					chars[len-1] = (char)val;
					if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
					else break;
				} else return NULL;
			}

			if (len > 0) if (write(mp->sock,chars,len)) len++; 
			g_free(chars);
		}
	}
	
	return NULL;
}

/* SetInputMode(mode) */
static DBusMessage *mp_dbus_set_input_mode(DBusMessage *msg, struct mp_state *mp, void *proc) {
	DBusError db_err; uint32_t u1;

	if (!mp || !proc) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&u1,DBUS_TYPE_INVALID);
	if (!dbus_error_is_set(&db_err)) mp_set_input_mode(mp->sock,(mp->mode = u1 ? 1 : 0));
	dbus_error_free(&db_err);
	return NULL;
}

static MPDBusMethodTable mp_methods[] = {
	{ "SetIndicator",   "uu",  "",          mp_dbus_generic_2u_method, G_DBUS_METHOD_FLAG_NOREPLY, mp_lcd_set_indicator },
	{ "BlinkOrBeep",    "uu",  "",          mp_dbus_generic_2u_method, G_DBUS_METHOD_FLAG_NOREPLY, mp_blink_or_beep },
	{ "SyncClock",      "",    "",          mp_dbus_generic_method,    G_DBUS_METHOD_FLAG_NOREPLY, mp_set_clock },
	{ "ClearScreen",    "",    "",          mp_dbus_generic_method,    G_DBUS_METHOD_FLAG_NOREPLY, mp_lcd_clear },
	{ "SetScreenMode",  "u",   "",          mp_dbus_generic_1u_method, G_DBUS_METHOD_FLAG_NOREPLY, mp_lcd_set_screen_mode },
	{ "SetDisplayMode", "uuu", "",          mp_dbus_generic_3u_method, G_DBUS_METHOD_FLAG_NOREPLY, mp_lcd_set_display_mode },
	{ "SetInputMode",   "u",   "",          mp_dbus_set_input_mode,    G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "GetKeyBindings", "",    "ayayayay",  mp_dbus_get_key_bindings,  0,                          NULL },
	{ "BindKey",        "uuu", "",          mp_dbus_bind_key,          G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "WriteRawData",   "ai",  "",          mp_dbus_write_raw_data,    G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "WriteText",      "s",   "",          mp_dbus_write_text,        G_DBUS_METHOD_FLAG_NOREPLY, NULL }, 
	{ "WriteLine",      "us",  "",          mp_dbus_write_line,        G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "WriteBuffer",    "us",  "",          mp_dbus_write_buffer,      G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "WriteTextBin",   "ai",  "",          mp_dbus_write_text_bin,    G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "WriteLineBin",   "uai", "",          mp_dbus_write_line_bin,    G_DBUS_METHOD_FLAG_NOREPLY, NULL },
	{ "WriteBufferBin", "uai", "",          mp_dbus_write_buffer_bin,  G_DBUS_METHOD_FLAG_NOREPLY, NULL }
};

static const char *introspect_ret = 
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"        <node name=\"" MP_DBUS_PATH "\">\n"
"          <interface name=\"" MP_DBUS_INTF "\">\n"
"            <method name=\"SetIndicator\">\n"
"              <!-- indicator: 1 (email) | 2 (IM) | 4 (Mute) | 8 (Alert)\n"
"                   show:      0 (hide)  | 1 (solid) | 2 (blink) -->\n"
"              <arg name=\"indicator\" type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"show\"      type=\"u\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"BlinkOrBeep\">\n"
"              <!-- beep_type: 0 (none) | 1 (low beep) | 2 (beep-beep) | 3 (short beep)\n"
"                   blink:     0 (no)   | 1 (yes) -->\n"
"              <arg name=\"beep_type\" type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"blink\"     type=\"u\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"BindKey\">\n"
"              <!-- scancode:  Mediapad scancode\n"
"                   mode:      0 (normal) | 1 (nav)\n"
"                   key:       key value to translate to (e.g. KEY_*) ] -->\n"
"              <arg name=\"scancode\" type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"mode\"     type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"key\"      type=\"u\" direction=\"in\"/>\n"
"           </method>\n"
"           <method name=\"GetKeyBindings\">\n"
"              <arg name=\"num_mode_keys\"       type=\"ay\" direction=\"out\"/>\n"
"              <arg name=\"nav_mode_keys\"       type=\"ay\" direction=\"out\"/>\n"
"              <arg name=\"num_mode_media_keys\" type=\"ay\" direction=\"out\"/>\n"
"              <arg name=\"nav_mode_media_keys\" type=\"ay\" direction=\"out\"/>\n"
"           </method>\n"
"           <method name=\"SyncClock\" />\n"
"           <method name=\"ClearScreen\" />\n"
"           <method name=\"SetScreenMode\">\n"
"              <!-- mode: 0 (clock) | 1 (text) -->\n"
"              <arg name=\"mode\"  type=\"u\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"SetDisplayMode\">\n"
"              <!-- mode1: Mode for line1 (LCD_DISP_MODE_*)\n"
"                   mode2: Mode for line2\n"
"                   mode3: Mode for line3 -->\n"
"              <arg name=\"mode1\" type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"mode2\" type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"mode3\" type=\"u\" direction=\"in\"/>\n"
"           </method>\n"
"           <method name=\"SetInputMode\">\n"
"              <!-- mode: 0 (numeric) | 1 (non-numeric) -->\n"
"              <arg name=\"mode\"  type=\"u\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteRawData\">\n"
"              <arg name=\"text\" type=\"ai\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteText\">\n"
"              <!-- Max Length: 144 -->\n"
"              <arg name=\"text\" type=\"s\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteLine\">\n"
"              <!-- Max Length: 48 -->\n"
"              <arg name=\"lineno\" type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"text\"   type=\"s\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteBuffer\">\n"
"              <!-- Max Length: 16 -->\n"
"              <arg name=\"bufno\"  type=\"u\" direction=\"in\"/>\n"
"              <arg name=\"text\"   type=\"s\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteTextBin\">\n"
"              <!-- Max Length: 144 -->\n"
"              <arg name=\"text\" type=\"ai\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteLineBin\">\n"
"              <!-- Max Length: 48 -->\n"
"              <arg name=\"lineno\" type=\"u\"  direction=\"in\"/>\n"
"              <arg name=\"text\"   type=\"ai\" direction=\"in\"/>\n"
"           </method>\n"
"            <method name=\"WriteBufferBin\">\n"
"              <!-- Max Length: 16 -->\n"
"              <arg name=\"bufno\"  type=\"u\"  direction=\"in\"/>\n"
"              <arg name=\"text\"   type=\"ai\" direction=\"in\"/>\n"
"           </method>\n"
"         </interface>\n"
"       </node>\n";

/* Handle a DBus message */
static DBusHandlerResult logitech_mediapad_msg(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessage *db_msg_reply; MPDBusMethodTable *method;
	struct mp_state *mp = (struct mp_state *)data;
	char *interface = (char *)dbus_message_get_interface(msg);

	/* Handle Introspection */
	if (strlen(interface) == 35 && !strncmp(interface,"org.freedesktop.DBus.Introspectable",35)) {
		if (!(db_msg_reply = dbus_message_new_method_return(msg)))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_append_args(db_msg_reply, DBUS_TYPE_STRING, &introspect_ret, DBUS_TYPE_INVALID);
		dbus_connection_send(conn,db_msg_reply,NULL);
		dbus_message_unref(db_msg_reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!mp) {
		error("logitech_mediapad_msg: mp is NULL!");
		dbus_message_unref(msg);
		dbus_connection_unref(conn);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* Check for a method call */
	for (method=mp_methods;method && method->name && method->function;method++) {
		if (!dbus_message_is_method_call(msg,MP_DBUS_INTF,method->name)) continue;
		if (!dbus_message_has_signature(msg,method->signature)) continue;

		debug("logitech_mediapad: Calling DBus method: %s\n",method->name);
		if (method->proc) db_msg_reply = method->function(msg,mp,method->proc);
		else              db_msg_reply = method->function(msg,mp,NULL);

		if (method->flags & G_DBUS_METHOD_FLAG_NOREPLY) {
			if (db_msg_reply) dbus_message_unref(db_msg_reply);
			db_msg_reply = dbus_message_new_method_return(msg);
			dbus_connection_send(conn,db_msg_reply,NULL);
			dbus_message_unref(db_msg_reply);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (!db_msg_reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_connection_send(conn,db_msg_reply,NULL);
		dbus_message_unref(db_msg_reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable mp_vtable = {
	.message_function    = &logitech_mediapad_msg,
	.unregister_function = NULL
};

/**************** UInput/fakehid Glue *******************/

/**
 * Initialize the mediapad
 */
int logitech_mediapad_setup_uinput(struct fake_input *fake_input, struct fake_hid *fake_hid) {
	DBusError db_err; struct uinput_dev dev; struct mp_state *mp; int i;
	
	/* Allocate a new mp_state struct */
	if (!(mp = g_new0(struct mp_state,1))) return 1;
	
	/* Open uinput */
	if ((mp->uinput = open("/dev/input/uinput",O_WRONLY|O_NONBLOCK)) <= 0) {
		if ((mp->uinput = open("/dev/uinput",O_WRONLY|O_NONBLOCK)) <= 0) {
			if ((mp->uinput = open("/dev/misc/uinput",O_WRONLY|O_NONBLOCK)) <= 0) {
				error("logitech_mediapad: Error opening uinput device!");
				g_free(mp);
				return 1;
			}
		}
	}

	/* Setup the uinput device */
	memset(&dev,0,sizeof(struct uinput_dev));
	snprintf(dev.name,sizeof(dev.name),"Logitech Mediapad");
	dev.id.bustype = BUS_BLUETOOTH;
	dev.id.vendor  = fake_hid->vendor;
	dev.id.product = fake_hid->product;

	if (write(mp->uinput,&dev,sizeof(struct uinput_dev)) != sizeof(struct uinput_dev)) {
		error("logitech_mediapad: Unable to create uinput device");
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Enable events */
	if (ioctl(mp->uinput,UI_SET_EVBIT,EV_KEY) < 0) {
		error("logitech_mediapad: Error enabling uinput key events");
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	if (ioctl(mp->uinput,UI_SET_EVBIT,EV_SYN) < 0) {
		error("logitech_mediapad: Error enabling uinput syn events");
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Enable keys */
	for (i=0;i<KEY_UNKNOWN;i++) {
		if (ioctl(mp->uinput,UI_SET_KEYBIT,i) < 0) {
			error("logitech_mediapad: Error enabling key #%d",i);
			close(mp->uinput);
			g_free(mp);
			return 1;
		}
	}
	
	/* Create the uinput device */
	if (ioctl(mp->uinput,UI_DEV_CREATE) < 0) {
		error("logitech_mediapad: Error creating uinput device");
		close(mp->uinput);
		g_free(mp);
		return 1;
	} 

	/* Get-on-D-Bus :P */
	dbus_error_init(&db_err);
	if (!(mp->db_conn = dbus_bus_get(DBUS_BUS_SYSTEM,&db_err))) {
		error("logitech_mediapad: Unable to connect to DBus.");
		dbus_error_free(&db_err);
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Request our interface */
	dbus_connection_set_exit_on_disconnect(mp->db_conn,FALSE);
	dbus_bus_request_name(mp->db_conn,MP_DBUS_INTF,DBUS_NAME_FLAG_REPLACE_EXISTING,&db_err);
	if (dbus_error_is_set(&db_err)) {
		error("logitech_mediapad: Failed to register mediapad interface on path %s",MP_DBUS_INTF);
		dbus_connection_unref(mp->db_conn);
		dbus_error_free(&db_err);
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Register our object path, and method table */
	if (!dbus_connection_register_object_path(mp->db_conn,MP_DBUS_PATH,&mp_vtable,mp)) 
			error("logitech_mediapad: Unable to register object path!");

	/* Get the interrupt socket */
	mp->sock           = g_io_channel_unix_get_fd(fake_input->io);
	fake_hid->priv     = mp;
	fake_input->uinput = mp->uinput;

	/* Set the mediapad clock, enable mode switch notifications. */
	mp_set_clock(mp->sock);
	mp_lcd_set_screen_mode(mp->sock,LCD_SCREEN_MODE_CLOCK);
	write_mpcmd(mp->sock,enable_mode_notification);
	return 0;
}

/**
 * Handle an event from the mediapad
 */
gboolean logitech_mediapad_event(GIOChannel *chan, GIOCondition cond, gpointer data) {
	int ln = 0, isk = 0; char buf[24], *cwtmp;
	struct fake_input *fake_input = (struct fake_input *)data;
	struct mp_state *mp = (struct mp_state *)(((struct fake_hid *)(fake_input->priv))->priv);
	isk = g_io_channel_unix_get_fd(chan);

	if (cond == G_IO_IN) {
		memset(buf,0,24);
		if ((ln = read(isk, buf, sizeof(buf))) <= 0) { 
			g_free(buf);
			g_io_channel_unref(chan);
			return FALSE;
		} 

		debug("dinovo: m %d: in: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x",
			  mp->mode,buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);

		/* Translate/Inject keypresses */
		if (buf[1] == 0x10 && buf[3] == 0x03) { /* Media keys */
			if (buf[4] != 0x00 && (buf[4] & 0xff) <= MP_INPUT_MODE_NUM) {
				/* Mode switch notification */
				mp->prev_key = 0;
				mp->mode     = (buf[4] == MP_INPUT_MODE_NAV) ? 1 : 0;
				if (buf[4] != MP_INPUT_MODE_CALC) mp_set_input_mode(isk,mp->mode);
				return TRUE;
			} else {
				switch (buf[4] & 0xff) {
					case 0x00: /* (Media) Key up event */
						if (!mp->discard_keyup) {
							if (mp->prev_key != 0) { 
								inject_key(mp->uinput,mp->prev_key,0);
								mp->prev_key = 0; 
							}
						} else mp->discard_keyup = 0; 
					break;
					case MP_KEY_MEDIA:
						switch (buf[5]) {
							case 0x01: /* Media key */
								mp->prev_key = translate_key(mp->mode,MP_KEY_MEDIA);
								inject_key(mp->uinput,mp->prev_key,1);
							break;
							case 0x02: /* Clear Screen key */
								mp_lcd_clear(isk);
								if (mp->icons & LCD_ICON_MUTE) { 
									mp->icons = LCD_ICON_MUTE; 
									mp_lcd_set_indicator(isk,LCD_ICON_MUTE,1); 
								}
							break;
						}
					break;
					case MP_KEY_FFWD:
					case MP_KEY_REW:
					case MP_KEY_STOP:
					case MP_KEY_PLAY:
						mp->prev_key = translate_key(mp->mode,buf[4]);
						inject_key(mp->uinput,mp->prev_key,1);
					break;
					case MP_KEY_MUTE:
						mp->prev_key = translate_key(mp->mode,MP_KEY_MUTE);
						mp->icons   ^= LCD_ICON_MUTE; 
						inject_key(mp->uinput,mp->prev_key,1);
						mp_lcd_set_indicator(isk,LCD_ICON_MUTE,(mp->icons & LCD_ICON_MUTE) ? 1 : 0);
					break;
					case MP_KEY_VOLUP:
					case MP_KEY_VOLDOWN:
						mp->prev_key = translate_key(mp->mode,buf[4]);  
						mp->icons   &= ~LCD_ICON_MUTE; 
						inject_key(mp->uinput,mp->prev_key,1);
						mp_lcd_set_indicator(isk,LCD_ICON_MUTE,0);
					break;
				}
			}
		} else if (buf[1] == 0x01 && buf[2] == 0x00) { /* Non-media keys */
			/* (Non-media) Key up event */
			if (buf[4] == 0x00 && buf[5] == 0x00 && mp->prev_key != 0) {
				inject_key(mp->uinput,mp->prev_key,0);
			} else if (buf[4] != 0x00) { /* Non-media key press */
				mp->prev_key = translate_key(mp->mode,buf[4] & 0x7f); 
				inject_key(mp->uinput,mp->prev_key,1); 
			}
		} else if (buf[1] == 0x11 && buf[3] == 0x0a) { 
			/* Calculator Result */
			debug("Got Calc result: %s",&buf[4]);
		}
	} else {
		if (mp->db_conn) {
			dbus_connection_unregister_object_path(mp->db_conn,MP_DBUS_INTF);
			dbus_connection_unref(mp->db_conn); 
		}
		g_free(mp); 
		return FALSE; 
	}
	return TRUE;
}
/* vi:set ts=4: */
