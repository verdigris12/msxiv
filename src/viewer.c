#include "viewer.h"
#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Xutil.h>
#include <MagickWand/MagickWand.h>

/* Command bar geometry and font selection */
#define CMD_BAR_HEIGHT 15
#define CMD_BAR_FONT   "monospace"   /* fallback to "fixed" if not found */

#define ZOOM_STEP 0.1
#define MIN_ZOOM 0.1
#define MAX_ZOOM 20.0

/* State for the currently displayed image. */
static MagickWand *g_wand = NULL;
static int g_img_width = 0;
static int g_img_height = 0;
static int g_pan_x = 0;
static int g_pan_y = 0;
static double g_zoom = 1.0;

/* Current filename + config pointer */
static char g_filename[1024] = {0};
static MsxivConfig *g_config = NULL;

/* Command line mode input */
static char g_command_input[1024] = {0};
static int g_command_mode = 0;
static int g_command_len = 0;

/* Colors/pixels for background, text, cmd bar. */
static unsigned long g_bg_pixel = 0;
static unsigned long g_text_pixel = 0;
static unsigned long g_cmdbar_bg_pixel = 0;

/* Font for the command bar text */
static XFontStruct *g_cmdFont = NULL;

/* WM_DELETE_WINDOW atom, for window close events. */
static Atom wmDeleteMessage;

/*
 * Forward declarations
 */
static void load_image(Display *dpy, Window win, const char *filename);
static void render_image(Display *dpy, Window win);

/*
 * Helper: Fit the image to the window, resetting pan/zoom.
 */
static void fit_zoom(Display *dpy, Window win)
{
	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	double sx = (double)win_w / (double)g_img_width;
	double sy = (double)win_h / (double)g_img_height;
	g_zoom = (sx < sy) ? sx : sy;
	g_pan_x = 0;
	g_pan_y = 0;
}

/*
 * Load a new file into the wand, reset zoom/pan, set g_filename, then render.
 */
static void load_image(Display *dpy, Window win, const char *filename)
{
	if (g_wand) {
		DestroyMagickWand(g_wand);
		g_wand = NULL;
	}
	g_wand = NewMagickWand();
	if (MagickReadImage(g_wand, filename) == MagickFalse) {
		fprintf(stderr, "Failed to read image: %s\n", filename);
		DestroyMagickWand(g_wand);
		g_wand = NULL;
		return;
	}

	strncpy(g_filename, filename, sizeof(g_filename) - 1);
	g_filename[sizeof(g_filename) - 1] = '\0';

	g_img_width = (int)MagickGetImageWidth(g_wand);
	g_img_height = (int)MagickGetImageHeight(g_wand);
	g_zoom = 1.0;
	g_pan_x = 0;
	g_pan_y = 0;

	render_image(dpy, win);
}

/*
 * Actually draw the image in the window, plus the command bar if in command mode.
 */
static void render_image(Display *dpy, Window win)
{
	if (!g_wand) {
		return;
	}
	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	GC gc = DefaultGC(dpy, DefaultScreen(dpy));
	/* Clear the background using g_bg_pixel. */
	XSetForeground(dpy, gc, g_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	double scaled_w = g_img_width * g_zoom;
	double scaled_h = g_img_height * g_zoom;

	MagickWand *tmp = CloneMagickWand(g_wand);
	/* Some ImageMagick builds want 4-arg, no 'blur' param. */
	MagickResizeImage(tmp, (size_t)scaled_w, (size_t)scaled_h, LanczosFilter);

	/* Clamp pan so we don't go off the edges. */
	if (g_pan_x > (int)scaled_w - 1) g_pan_x = (int)scaled_w - 1;
	if (g_pan_y > (int)scaled_h - 1) g_pan_y = (int)scaled_h - 1;
	if (g_pan_x < 0) g_pan_x = 0;
	if (g_pan_y < 0) g_pan_y = 0;

	int copy_w = (win_w > (int)scaled_w) ? (int)scaled_w : win_w;
	int copy_h = (win_h > (int)scaled_h) ? (int)scaled_h : win_h;

	MagickCropImage(tmp, copy_w, copy_h, g_pan_x, g_pan_y);
	MagickSetImageFormat(tmp, "RGBA");

	size_t out_w = MagickGetImageWidth(tmp);
	size_t out_h = MagickGetImageHeight(tmp);
	size_t length = out_w * out_h * 4;
	unsigned char *blob = MagickGetImageBlob(tmp, &length);

	XImage *ximg = XCreateImage(dpy,
	                            DefaultVisual(dpy, DefaultScreen(dpy)),
	                            24, ZPixmap,
	                            0,
	                            (char *)malloc(out_w * out_h * 4),
	                            out_w, out_h,
	                            32, 0);
	if (!ximg) {
		MagickRelinquishMemory(blob);
		DestroyMagickWand(tmp);
		return;
	}
	memcpy(ximg->data, blob, out_w * out_h * 4);

	/* Center the image if it's smaller than the window. */
	int pos_x = 0;
	int pos_y = 0;
	if ((int)out_w < win_w) {
		pos_x = (win_w - (int)out_w) / 2;
	}
	if ((int)out_h < win_h) {
		pos_y = (win_h - (int)out_h) / 2;
	}

	XPutImage(dpy, win, gc, ximg, 0, 0, pos_x, pos_y, out_w, out_h);

	XFree(ximg);
	MagickRelinquishMemory(blob);
	DestroyMagickWand(tmp);

	/* If in command mode, draw a black bar + white text at the bottom. */
	if (g_command_mode) {
		int bar_y = win_h - CMD_BAR_HEIGHT;

		XSetForeground(dpy, gc, g_cmdbar_bg_pixel);
		XFillRectangle(dpy, win, gc, 0, bar_y, win_w, CMD_BAR_HEIGHT);

		char display_cmd[1024];
		snprintf(display_cmd, sizeof(display_cmd), ":%s", g_command_input);

		XSetForeground(dpy, gc, g_text_pixel);
		if (g_cmdFont) {
			XSetFont(dpy, gc, g_cmdFont->fid);
		}

		/* We'll place text a little up from the bar's bottom. */
		int text_x = 5;
		int text_y = bar_y + CMD_BAR_HEIGHT - 3;

		XDrawString(dpy, win, gc, text_x, text_y,
		            display_cmd, strlen(display_cmd));
	}
}

/*
 * viewer_init() sets up X, loads background color, loads first image if any, etc.
 */
int viewer_init(Display **dpy, Window *win, ViewerData *vdata, MsxivConfig *config)
{
	MagickWandGenesis();
	g_config = config;
	g_wand = NULL;

	*dpy = XOpenDisplay(NULL);
	if (!*dpy) {
		fprintf(stderr, "Cannot open display\n");
		return -1;
	}
	int screen = DefaultScreen(*dpy);

	*win = XCreateSimpleWindow(
		*dpy, RootWindow(*dpy, screen),
		0, 0, 800, 600,
		1,
		BlackPixel(*dpy, screen),
		WhitePixel(*dpy, screen)
	);

	XSelectInput(*dpy, *win,
	             ExposureMask |
	             KeyPressMask |
	             ButtonPressMask |
	             ButtonReleaseMask |
	             PointerMotionMask |
	             StructureNotifyMask);

	/* So we can detect window close. */
	wmDeleteMessage = XInternAtom(*dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(*dpy, *win, &wmDeleteMessage, 1);

	XMapWindow(*dpy, *win);

	/* Wait for MapNotify */
	XEvent e;
	while (1) {
		XNextEvent(*dpy, &e);
		if (e.type == MapNotify) {
			break;
		}
	}

	{
		Colormap cmap = DefaultColormap(*dpy, screen);
		XColor xcol;
		/* background color from config->bg_color */
		if (XParseColor(*dpy, cmap, config->bg_color, &xcol) &&
		    XAllocColor(*dpy, cmap, &xcol)) {
			g_bg_pixel = xcol.pixel;
		} else {
			/* fallback if parse fails */
			g_bg_pixel = BlackPixel(*dpy, screen);
		}
	}

	g_text_pixel = WhitePixel(*dpy, screen);

	/* force black for the command bar background */
	{
		Colormap cmap = DefaultColormap(*dpy, screen);
		XColor xcol;
		if (XParseColor(*dpy, cmap, "#000000", &xcol) &&
		    XAllocColor(*dpy, cmap, &xcol)) {
			g_cmdbar_bg_pixel = xcol.pixel;
		} else {
			g_cmdbar_bg_pixel = BlackPixel(*dpy, screen);
		}
	}

	/* Load the monospace font. */
	g_cmdFont = XLoadQueryFont(*dpy, CMD_BAR_FONT);
	if (!g_cmdFont) {
		g_cmdFont = XLoadQueryFont(*dpy, "fixed");
	}

	/* Load first image if we have multiple. */
	if (vdata->fileCount > 0) {
		load_image(*dpy, *win, vdata->files[vdata->currentIndex]);
	}

	return 0;
}

/*
 * The main event loop. 
 * KeyPress now uses XLookupString to detect typed ASCII (including ':').
 */
void viewer_run(Display *dpy, Window win, ViewerData *vdata)
{
	XEvent ev;
	int is_ctrl_pressed = 0;

	while (1) {
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case Expose:
		case ConfigureNotify:
			render_image(dpy, win);
			break;
		case ClientMessage:
			/* WM_DELETE_WINDOW => exit. */
			if ((Atom)ev.xclient.data.l[0] == wmDeleteMessage) {
				return;
			}
			break;
		case DestroyNotify:
			return;
		case KeyPress: {
			/*
			 * We use XLookupString to detect typed ASCII.
			 * Then we also check KeySym for special keys like arrow, space, backspace.
			 */
			char buf[32];
			KeySym ks;
			XComposeStatus comp;
			int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, &comp);
			buf[len] = '\0';

			if (g_command_mode) {
				/*
				 * If we are in command mode, typed ASCII goes into the command buffer,
				 * unless it's Enter, BackSpace, or Escape, etc.
				 */
				if (ks == XK_Return) {
					/* finalize command */
					g_command_input[g_command_len] = '\0';
					g_command_mode = 0;

					/* parse command */
					if (strncmp(g_command_input, "save", 4) == 0) {
						cmd_save(g_filename);
					} else if (strncmp(g_command_input, "convert", 7) == 0) {
						cmd_convert(g_filename);
					} else if (strncmp(g_command_input, "delete", 6) == 0) {
						cmd_delete(g_filename);
					} else if (strncmp(g_command_input, "bookmark", 8) == 0) {
						char *lbl = g_command_input + 8;
						while (*lbl == ' ' || *lbl == '\t') lbl++;
						cmd_bookmark(g_filename, lbl, g_config);
					}
					g_command_len = 0;
					g_command_input[0] = '\0';
					render_image(dpy, win);
				} else if (ks == XK_BackSpace || ks == XK_Delete) {
					if (g_command_len > 0) {
						g_command_len--;
						g_command_input[g_command_len] = '\0';
					}
					render_image(dpy, win);
				} else if (ks == XK_Escape) {
					g_command_mode = 0;
					g_command_len = 0;
					g_command_input[0] = '\0';
					render_image(dpy, win);
				} else {
					/* Add typed ASCII to command buffer if it's printable. */
					if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
						if (g_command_len < (int)(sizeof(g_command_input) - 1)) {
							g_command_input[g_command_len++] = buf[0];
							g_command_input[g_command_len] = '\0';
						}
					}
					render_image(dpy, win);
				}
				break;
			} else {
				/* Not in command mode. Check if user typed ":" => enter command mode. */
				if (len == 1 && buf[0] == ':') {
					g_command_mode = 1;
					g_command_len = 0;
					g_command_input[0] = '\0';
					render_image(dpy, win);
					break;
				}
				/*
				 * Otherwise, handle non-ASCII or special keys (like arrow keys) via ks.
				 */
				is_ctrl_pressed = (ev.xkey.state & ControlMask) != 0;

				switch (ks) {
				case XK_q:
					return;
				case XK_space:
					/* Next image if available */
					if (vdata->currentIndex < vdata->fileCount - 1) {
						vdata->currentIndex++;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					}
					break;
				case XK_BackSpace:
					/* Previous image if available */
					if (vdata->currentIndex > 0) {
						vdata->currentIndex--;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					}
					break;
				case XK_w:
				case XK_Up:
					g_pan_y -= 50;
					render_image(dpy, win);
					break;
				case XK_s:
				case XK_Down:
					g_pan_y += 50;
					render_image(dpy, win);
					break;
				case XK_a:
				case XK_Left:
					g_pan_x -= 50;
					render_image(dpy, win);
					break;
				case XK_d:
				case XK_Right:
					g_pan_x += 50;
					render_image(dpy, win);
					break;
				case XK_plus:
				case XK_equal:
					/* '=' without shift => fit window. */
					if (ks == XK_equal && !(ev.xkey.state & ShiftMask)) {
						fit_zoom(dpy, win);
					} else {
						g_zoom += ZOOM_STEP;
						if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
					}
					render_image(dpy, win);
					break;
				case XK_minus:
					g_zoom -= ZOOM_STEP;
					if (g_zoom < MIN_ZOOM) g_zoom = MIN_ZOOM;
					render_image(dpy, win);
					break;
				default:
					/*
					 * Possibly a config-defined keybinding:
					 * we compare the KeySym to user-defined strings,
					 * or if len>0 and it's a single ASCII char, we can match that.
					 */
					for (int i = 0; i < g_config->keybind_count; i++) {
						char pressed_key[32];
						/* If we got a single ASCII char, use that. Otherwise, fallback to numeric. */
						if (len == 1 && (buf[0] >= 32 && buf[0] < 127)) {
							snprintf(pressed_key, sizeof(pressed_key), "%c", buf[0]);
						} else {
							/* store the KeySym in decimal */
							snprintf(pressed_key, sizeof(pressed_key), "%lu", (unsigned long)ks);
						}
						if (strcmp(g_config->keybinds[i].key, pressed_key) == 0) {
							if (strncmp(g_config->keybinds[i].action, "save", 4) == 0) {
								cmd_save(g_filename);
							} else if (strncmp(g_config->keybinds[i].action, "convert", 7) == 0) {
								cmd_convert(g_filename);
							} else if (strncmp(g_config->keybinds[i].action, "delete", 6) == 0) {
								cmd_delete(g_filename);
							} else if (strncmp(g_config->keybinds[i].action, "bookmark", 8) == 0) {
								char *lbl = (char*)g_config->keybinds[i].action + 8;
								while (*lbl == ' ' || *lbl == '\t') lbl++;
								cmd_bookmark(g_filename, lbl, g_config);
							} else if (strncmp(g_config->keybinds[i].action, "exec ", 5) == 0) {
								char syscmd[1024];
								const char *templ = g_config->keybinds[i].action + 5;
								char *p = syscmd;
								const char *t = templ;
								while (*t && (p - syscmd) < (int)(sizeof(syscmd) - 1)) {
									if (*t == '%' && *(t+1) == 's') {
										snprintf(p, sizeof(syscmd) - (p - syscmd), "%s", g_filename);
										p += strlen(g_filename);
										t += 2;
									} else {
										*p++ = *t++;
									}
								}
								*p = '\0';
								(void)system(syscmd);
							}
							render_image(dpy, win);
						}
					}
					break;
				} /* end switch(ks) */
			} /* end else (not in command mode) */
		} break; /* end KeyPress */

		case ButtonPress:
			/* Ctrl + mouse wheel => zoom. */
			if (ev.xbutton.button == 4) {
				if (is_ctrl_pressed) {
					g_zoom += ZOOM_STEP;
					if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
					g_pan_x = (int)((ev.xbutton.x + g_pan_x) * (1 + ZOOM_STEP)) - ev.xbutton.x;
					g_pan_y = (int)((ev.xbutton.y + g_pan_y) * (1 + ZOOM_STEP)) - ev.xbutton.y;
					render_image(dpy, win);
				}
			} else if (ev.xbutton.button == 5) {
				if (is_ctrl_pressed) {
					g_zoom -= ZOOM_STEP;
					if (g_zoom < MIN_ZOOM) g_zoom = MIN_ZOOM;
					g_pan_x = (int)((ev.xbutton.x + g_pan_x) * (1 - ZOOM_STEP)) - ev.xbutton.x;
					g_pan_y = (int)((ev.xbutton.y + g_pan_y) * (1 - ZOOM_STEP)) - ev.xbutton.y;
					render_image(dpy, win);
				}
			}
			break;
		case ButtonRelease:
			break;
		case MotionNotify:
			/* Implement drag panning if desired. */
			break;
		} /* end switch(ev.type) */
	}
}

/*
 * viewer_cleanup: free wand, terminate Magick, close X display.
 */
void viewer_cleanup(Display *dpy)
{
	if (g_wand) {
		DestroyMagickWand(g_wand);
		g_wand = NULL;
	}
	MagickWandTerminus();

	/* Optionally free the font: 
	 * if (g_cmdFont) {
	 *     XFreeFont(dpy, g_cmdFont);
	 * }
	 */

	if (dpy) {
		XCloseDisplay(dpy);
	}
}

