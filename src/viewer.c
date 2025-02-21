#include "viewer.h"
#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Xutil.h>
/* Removed XFixes / Xrender includes, as you said you commented them out:
// #include <X11/extensions/Xfixes.h>
// #include <X11/extensions/Xrender.h>
*/

#include <MagickWand/MagickWand.h>

#define ZOOM_STEP 0.1
#define MIN_ZOOM 0.1
#define MAX_ZOOM 20.0

static MagickWand *g_wand = NULL;
static int g_img_width = 0;
static int g_img_height = 0;
static int g_pan_x = 0;
static int g_pan_y = 0;
static double g_zoom = 1.0;

static char g_filename[1024] = {0};
static MsxivConfig *g_config = NULL;

/* We'll store the background pixel in a global after we parse the color. */
static unsigned long g_bg_pixel = 0;

/* For the : command line input */
static char g_command_input[1024] = {0};
static int g_command_mode = 0;
static int g_command_len = 0;

static void render_image(Display *dpy, Window win)
{
	if (!g_wand) {
		return;
	}

	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);

	int win_w = xwa.width;
	int win_h = xwa.height;

	/* Clear the background to user-specified color */
	GC gc = DefaultGC(dpy, DefaultScreen(dpy));
	XSetForeground(dpy, gc, g_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	/* Now draw the scaled image. */
	double scaled_w = g_img_width * g_zoom;
	double scaled_h = g_img_height * g_zoom;

	MagickWand *tmp_wand = CloneMagickWand(g_wand);
	/* 4-argument call for some IM7 builds: */
	MagickResizeImage(tmp_wand, (size_t)scaled_w, (size_t)scaled_h, LanczosFilter);

	/* Clamp pan so we don't scroll out of bounds */
	if (g_pan_x > (int)scaled_w - 1) g_pan_x = (int)scaled_w - 1;
	if (g_pan_y > (int)scaled_h - 1) g_pan_y = (int)scaled_h - 1;
	if (g_pan_x < 0) g_pan_x = 0;
	if (g_pan_y < 0) g_pan_y = 0;

	int copy_w = (win_w > (int)scaled_w) ? (int)scaled_w : win_w;
	int copy_h = (win_h > (int)scaled_h) ? (int)scaled_h : win_h;

	MagickCropImage(tmp_wand, copy_w, copy_h, g_pan_x, g_pan_y);
	MagickSetImageFormat(tmp_wand, "RGBA");

	size_t out_width = MagickGetImageWidth(tmp_wand);
	size_t out_height = MagickGetImageHeight(tmp_wand);
	size_t length = out_width * out_height * 4;
	unsigned char *blob = MagickGetImageBlob(tmp_wand, &length);

	/* Create XImage for the final region. */
	XImage *ximg = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
	                            ZPixmap, 0,
	                            (char *)malloc(out_width * out_height * 4),
	                            out_width, out_height, 32, 0);
	if (!ximg) {
		MagickRelinquishMemory(blob);
		DestroyMagickWand(tmp_wand);
		return;
	}

	memcpy(ximg->data, blob, out_width * out_height * 4);

	/* CHANGED: We center the image if smaller than window. */
	int pos_x = 0;
	int pos_y = 0;
	if ((int)out_width < win_w) {
		pos_x = (win_w - (int)out_width) / 2;
	}
	if ((int)out_height < win_h) {
		pos_y = (win_h - (int)out_height) / 2;
	}

	XPutImage(dpy, win, gc, ximg, 0, 0, pos_x, pos_y, out_width, out_height);

	XFree(ximg);
	MagickRelinquishMemory(blob);
	DestroyMagickWand(tmp_wand);
}

static void fit_zoom(Display *dpy, Window win)
{
	if (!g_wand) return;

	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);

	int win_w = xwa.width;
	int win_h = xwa.height;

	double scale_x = (double)win_w / (double)g_img_width;
	double scale_y = (double)win_h / (double)g_img_height;
	g_zoom = scale_x < scale_y ? scale_x : scale_y;

	g_pan_x = 0;
	g_pan_y = 0;
}

int viewer_init(Display **dpy, Window *win, const char *filename, MsxivConfig *config)
{
	MagickWandGenesis();

	g_config = config;
	strncpy(g_filename, filename, sizeof(g_filename) - 1);

	g_wand = NewMagickWand();
	if (MagickReadImage(g_wand, filename) == MagickFalse) {
		fprintf(stderr, "Failed to read image: %s\n", filename);
		DestroyMagickWand(g_wand);
		g_wand = NULL;
		return -1;
	}

	g_img_width = (int)MagickGetImageWidth(g_wand);
	g_img_height = (int)MagickGetImageHeight(g_wand);
	g_zoom = 1.0;
	g_pan_x = 0;
	g_pan_y = 0;

	/* OPEN DISPLAY */
	*dpy = XOpenDisplay(NULL);
	if (!*dpy) {
		fprintf(stderr, "Cannot open display\n");
		return -1;
	}
	int screen = DefaultScreen(*dpy);
	int win_w = (g_img_width < 800) ? g_img_width : 800;
	int win_h = (g_img_height < 600) ? g_img_height : 600;

	*win = XCreateSimpleWindow(*dpy,
	                           RootWindow(*dpy, screen),
	                           0, 0,
	                           win_w, win_h,
	                           1,
	                           BlackPixel(*dpy, screen),
	                           WhitePixel(*dpy, screen));

	XSelectInput(*dpy, *win,
	             ExposureMask |
	             KeyPressMask |
	             ButtonPressMask |
	             ButtonReleaseMask |
	             PointerMotionMask |
	             StructureNotifyMask);
	XMapWindow(*dpy, *win);

	/* WAIT FOR MAP */
	XEvent e;
	while (1) {
		XNextEvent(*dpy, &e);
		if (e.type == MapNotify)
			break;
	}

	/* CHANGED: parse & allocate background color from config->bg_color. */
	{
		Colormap cmap = DefaultColormap(*dpy, screen);
		XColor xcol;
		if (XParseColor(*dpy, cmap, config->bg_color, &xcol) &&
		    XAllocColor(*dpy, cmap, &xcol)) {
			g_bg_pixel = xcol.pixel;
		} else {
			/* If that fails, fallback to black. */
			g_bg_pixel = BlackPixel(*dpy, screen);
		}
	}

	return 0;
}

void viewer_run(Display *dpy, Window win)
{
	render_image(dpy, win);

	XEvent ev;
	int is_ctrl_pressed = 0;

	while (1) {
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case Expose:
			render_image(dpy, win);
			break;
		case ConfigureNotify:
			render_image(dpy, win);
			break;
		case KeyPress: {
			KeySym ks = XLookupKeysym(&ev.xkey, 0);

			/* Handle command mode (:) input first. */
			if (g_command_mode) {
				if (ks == XK_Return) {
					g_command_input[g_command_len] = '\0';
					g_command_mode = 0;

					if (strncmp(g_command_input, "save", 4) == 0) {
						cmd_save(g_filename);
					} else if (strncmp(g_command_input, "convert", 7) == 0) {
						cmd_convert(g_filename);
					} else if (strncmp(g_command_input, "delete", 6) == 0) {
						cmd_delete(g_filename);
					} else if (strncmp(g_command_input, "bookmark", 8) == 0) {
						char *label = g_command_input + 8;
						while (*label == ' ' || *label == '\t') label++;
						cmd_bookmark(g_filename, label, g_config);
					}

					g_command_len = 0;
					g_command_input[0] = '\0';
					render_image(dpy, win);
				} else if (ks == XK_BackSpace || ks == XK_Delete) {
					if (g_command_len > 0) {
						g_command_len--;
						g_command_input[g_command_len] = '\0';
					}
				} else if (ks == XK_Escape) {
					g_command_mode = 0;
					g_command_len = 0;
					g_command_input[0] = '\0';
				} else {
					char c = (char)XLookupKeysym(&ev.xkey, 0);
					if (c >= 32 && c < 127 &&
					    g_command_len < (int)(sizeof(g_command_input) - 1)) {
						g_command_input[g_command_len++] = c;
						g_command_input[g_command_len] = '\0';
					}
				}
				break;
			}

			/* If user pressed ':', enter command mode. */
			if (ks == XK_colon) {
				g_command_mode = 1;
				g_command_len = 0;
				g_command_input[0] = '\0';
				break;
			}

			is_ctrl_pressed = (ev.xkey.state & ControlMask) != 0;

			/* Normal panning/zooming or config key binds. */
			switch (ks) {
			case XK_q:
				return;
			case XK_w:
			case XK_Up:
				g_pan_y -= 50;
				break;
			case XK_s:
			case XK_Down:
				g_pan_y += 50;
				break;
			case XK_a:
			case XK_Left:
				g_pan_x -= 50;
				break;
			case XK_d:
			case XK_Right:
				g_pan_x += 50;
				break;
			case XK_plus:
			case XK_equal:
				if (ks == XK_equal && !(ev.xkey.state & ShiftMask)) {
					/* '=' without shift => fit to window. */
					fit_zoom(dpy, win);
					break;
				}
				/* otherwise, it's '+' */
				g_zoom += ZOOM_STEP;
				if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
				break;
			case XK_minus:
				g_zoom -= ZOOM_STEP;
				if (g_zoom < MIN_ZOOM) g_zoom = MIN_ZOOM;
				break;
			default: {
				/* Check config keybinds. */
				char pressed_key[32];
				if (ks >= XK_space && ks <= XK_asciitilde) {
					snprintf(pressed_key, sizeof(pressed_key), "%c", (char)ks);
				} else {
					snprintf(pressed_key, sizeof(pressed_key), "%lu", (unsigned long)ks);
				}
				for (int i = 0; i < g_config->keybind_count; i++) {
					if (strcmp(g_config->keybinds[i].key, pressed_key) == 0) {
						if (strncmp(g_config->keybinds[i].action, "save", 4) == 0) {
							cmd_save(g_filename);
						} else if (strncmp(g_config->keybinds[i].action, "convert", 7) == 0) {
							cmd_convert(g_filename);
						} else if (strncmp(g_config->keybinds[i].action, "delete", 6) == 0) {
							cmd_delete(g_filename);
						} else if (strncmp(g_config->keybinds[i].action, "bookmark", 8) == 0) {
							char *label = (char *)g_config->keybinds[i].action + 8;
							while (*label == ' ' || *label == '\t') label++;
							cmd_bookmark(g_filename, label, g_config);
						} else if (strncmp(g_config->keybinds[i].action, "exec ", 5) == 0) {
							/* run shell command, ignoring return code. */
							char cmd[1024];
							const char *templ = g_config->keybinds[i].action + 5;
							char *p = cmd;
							const char *t = templ;
							while (*t && (p - cmd) < (int)(sizeof(cmd) - 1)) {
								if (*t == '%' && *(t+1) == 's') {
									snprintf(p, sizeof(cmd) - (p - cmd), "%s", g_filename);
									p += strlen(g_filename);
									t += 2;
								} else {
									*p++ = *t++;
								}
							}
							*p = '\0';
							(void)system(cmd);
						}
					}
				}
			} break;
			}
			render_image(dpy, win);
		} break;

		case ButtonPress:
			/* Ctrl + wheel for zoom. */
			if (ev.xbutton.button == 4) {
				if (is_ctrl_pressed) {
					g_zoom += ZOOM_STEP;
					if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
					g_pan_x = (int)((ev.xbutton.x + g_pan_x) * (1 + ZOOM_STEP)) - ev.xbutton.x;
					g_pan_y = (int)((ev.xbutton.y + g_pan_y) * (1 + ZOOM_STEP)) - ev.xbutton.y;
				}
			} else if (ev.xbutton.button == 5) {
				if (is_ctrl_pressed) {
					g_zoom -= ZOOM_STEP;
					if (g_zoom < MIN_ZOOM) g_zoom = MIN_ZOOM;
					g_pan_x = (int)((ev.xbutton.x + g_pan_x) * (1 - ZOOM_STEP)) - ev.xbutton.x;
					g_pan_y = (int)((ev.xbutton.y + g_pan_y) * (1 - ZOOM_STEP)) - ev.xbutton.y;
				}
			}
			render_image(dpy, win);
			break;

		case ButtonRelease:
			break;

		case MotionNotify:
			/* If you want click-drag panning, track the mouse here. */
			break;
		}
	}
}

void viewer_cleanup(Display *dpy)
{
	if (g_wand) {
		DestroyMagickWand(g_wand);
		g_wand = NULL;
	}
	MagickWandTerminus();

	if (dpy) {
		XCloseDisplay(dpy);
	}
}
