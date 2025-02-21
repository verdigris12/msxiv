#include "viewer.h"
#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

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

/* Built-in commands we want to tab-complete. */
static const char *g_builtin_cmds[] = {
	"save",
	"convert",
	"delete",
	"bookmark",
	NULL
};

/* Forward declarations */
static void load_image(Display *dpy, Window win, const char *filename);
static void render_image(Display *dpy, Window win);

/*
 * Fit the image to the window, resetting pan/zoom.
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
 * load_image: read the new file into the wand, reset zoom/pan, draw it.
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
 * Minimal helper for "~" expansion if needed (for path completion).
 */
static void expand_tilde(const char *in, char *out, size_t outsize)
{
	if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
		const char *home = getenv("HOME");
		if (!home) home = ".";
		snprintf(out, outsize, "%s%s", home, in + 1);
	} else {
		snprintf(out, outsize, "%s", in);
	}
}

/*
 * Attempt to tab-complete a built-in command (save, convert, delete, bookmark).
 * If there's a single match or longer prefix, fill it in.
 */
static int complete_builtin_cmd(char *buffer, size_t bufsz)
{
	/* skip leading ':' */
	char *cmdstart = buffer + 1;
	char *space = strchr(cmdstart, ' ');
	if (space) {
		/* user typed full command plus maybe arguments => no command completion now */
		return 0;
	}
	/* partial command typed */
	const char *partial = cmdstart;
	int partial_len = strlen(partial);

	int matches_found = 0;
	const char *best_match = NULL;
	int common_prefix_len = -1;

	for (int i = 0; g_builtin_cmds[i]; i++) {
		const char *cand = g_builtin_cmds[i];
		if (strncmp(cand, partial, partial_len) == 0) {
			matches_found++;
			if (!best_match) {
				best_match = cand;
				common_prefix_len = (int)strlen(cand);
			} else {
				int c = 0;
				while (best_match[c] && cand[c] && best_match[c] == cand[c]) {
					c++;
				}
				if (c < common_prefix_len) {
					common_prefix_len = c;
				}
			}
		}
	}
	if (matches_found == 0) {
		return 0;
	}
	if (matches_found == 1) {
		/* fill entire command */
		snprintf(cmdstart, bufsz - 1, "%s", best_match);
		return 1;
	} else {
		/* multiple matches => fill up to the common prefix among them */
		if (common_prefix_len <= partial_len) {
			return 0; /* no improvement */
		}
		memcpy(cmdstart + partial_len, best_match + partial_len, common_prefix_len - partial_len);
		cmdstart[common_prefix_len] = '\0';
		return 1;
	}
}

/*
 * Attempt to tab-complete a path argument after a known command, e.g. ":save /some/pa<Tab>" 
 */
static int complete_path(char *buffer, size_t bufsz)
{
	/* find the space => parse partial path. */
	char *space = strchr(buffer, ' ');
	if (!space) {
		return 0;
	}
	char *pathstart = space;
	while (*pathstart == ' ' || *pathstart == '\t') {
		pathstart++;
	}
	if (*pathstart == '\0') {
		/* no partial typed => do nothing */
		return 0;
	}

	char expanded[1024];
	expand_tilde(pathstart, expanded, sizeof(expanded));

	/* separate directory from partial base. */
	char dir[1024];
	char base[1024];
	snprintf(dir, sizeof(dir), "%s", expanded);
	char *last_slash = strrchr(dir, '/');
	if (!last_slash) {
		snprintf(dir, sizeof(dir), ".");
		snprintf(base, sizeof(base), "%s", expanded);
	} else {
		*last_slash = '\0';
		snprintf(base, sizeof(base), "%s", last_slash + 1);
		if (*dir == '\0') {
			snprintf(dir, sizeof(dir), "/");
		}
	}

	DIR *dp = opendir(dir);
	if (!dp) {
		return 0;
	}

	int matches = 0;
	char best_match[1024] = {0};
	int best_match_len = 0;
	int common_prefix_len = -1;

	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (strncmp(de->d_name, base, strlen(base)) == 0) {
			matches++;
			if (matches == 1) {
				snprintf(best_match, sizeof(best_match), "%s", de->d_name);
				best_match_len = (int)strlen(best_match);
				common_prefix_len = best_match_len;
			} else {
				int c = 0;
				while (best_match[c] && de->d_name[c] && best_match[c] == de->d_name[c]) {
					c++;
				}
				if (c < common_prefix_len) {
					common_prefix_len = c;
				}
			}
		}
	}
	closedir(dp);

	if (matches == 0) {
		return 0;
	} else if (matches == 1) {
		char fullpath[1024];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, best_match);
		struct stat st;
		if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
			strcat(best_match, "/");
		}
	} else {
		if (common_prefix_len <= (int)strlen(base)) {
			return 0;
		}
		best_match[common_prefix_len] = '\0';
	}

	char final[1024];
	if (strcmp(dir, "/") == 0) {
		snprintf(final, sizeof(final), "/%s", best_match);
	} else {
		snprintf(final, sizeof(final), "%s/%s", dir, best_match);
	}

	int prefix_len = (int)(pathstart - buffer);
	snprintf(pathstart, bufsz - prefix_len, "%s", final);

	return 1;
}

/*
 * Attempt to do tab-completion in command mode.
 * If there's no space => command completion, else path completion.
 */
static void do_tab_completion(void)
{
	if (g_command_len <= 1) {
		return;
	}
	char *space = strchr(g_command_input, ' ');
	if (!space) {
		/* command completion */
		int improved = complete_builtin_cmd(g_command_input, sizeof(g_command_input));
		if (improved) {
			g_command_len = strlen(g_command_input);
		}
	} else {
		/* path completion */
		int improved = complete_path(g_command_input, sizeof(g_command_input));
		if (improved) {
			g_command_len = strlen(g_command_input);
		}
	}
}

/*
 * Actually draw the image and, if in command mode, draw the bar at the bottom.
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
	/* Clear background */
	XSetForeground(dpy, gc, g_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	double scaled_w = g_img_width * g_zoom;
	double scaled_h = g_img_height * g_zoom;

	MagickWand *tmp = CloneMagickWand(g_wand);
	MagickResizeImage(tmp, (size_t)scaled_w, (size_t)scaled_h, LanczosFilter);

	/* Pan clamp */
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

	/* Center if smaller than window */
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

	/* If in command mode, draw bar + text at bottom. */
	if (g_command_mode) {
		int bar_y = win_h - CMD_BAR_HEIGHT;

		XSetForeground(dpy, gc, g_cmdbar_bg_pixel);
		XFillRectangle(dpy, win, gc, 0, bar_y, win_w, CMD_BAR_HEIGHT);

		XSetForeground(dpy, gc, g_text_pixel);
		if (g_cmdFont) {
			XSetFont(dpy, gc, g_cmdFont->fid);
		}
		int text_x = 5;
		int text_y = bar_y + CMD_BAR_HEIGHT - 3;

		XDrawString(dpy, win, gc, text_x, text_y,
		            g_command_input, strlen(g_command_input));
	}
}

/*
 * viewer_init: open display, create window, set up, load first file if any
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
		if (XParseColor(*dpy, cmap, config->bg_color, &xcol) &&
		    XAllocColor(*dpy, cmap, &xcol)) {
			g_bg_pixel = xcol.pixel;
		} else {
			g_bg_pixel = BlackPixel(*dpy, screen);
		}
	}

	g_text_pixel = WhitePixel(*dpy, screen);

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

	/* Load monospace font. */
	g_cmdFont = XLoadQueryFont(*dpy, CMD_BAR_FONT);
	if (!g_cmdFont) {
		g_cmdFont = XLoadQueryFont(*dpy, "fixed");
	}

	/* If multiple images => load first. */
	if (vdata->fileCount > 0) {
		load_image(*dpy, *win, vdata->files[vdata->currentIndex]);
	}

	return 0;
}

/*
 * viewer_run: main event loop
 */
void viewer_run(Display *dpy, Window win, ViewerData *vdata)
{
	XEvent ev;
	int is_ctrl_pressed = 0;

	/* Optional: keep command_input empty until user types ':' 
	   or we can initialize with a single colon. 
	   We'll keep it empty to avoid confusion:
	*/
	g_command_input[0] = '\0';
	g_command_len = 0;

	while (1) {
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case Expose:
		case ConfigureNotify:
			render_image(dpy, win);
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wmDeleteMessage) {
				return;
			}
			break;
		case DestroyNotify:
			return;
		case KeyPress: {
			char buf[32];
			KeySym ks;
			XComposeStatus comp;
			int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, &comp);
			buf[len] = '\0';

			if (g_command_mode) {
				if (ks == XK_Return) {
					g_command_input[g_command_len] = '\0';
					g_command_mode = 0;
					/* parse command, skipping leading colon if present */
					char *cmdline = g_command_input;
					if (cmdline[0] == ':') {
						cmdline++;
					}

					/* --- SHIFT: save => check if there's a path after. --- */
					if (strncmp(cmdline, "save", 4) == 0) {
						char *dest = cmdline + 4;
						while (*dest == ' ' || *dest == '\t') {
							dest++;
						}
						if (*dest) {
							/* user typed a path => call cmd_save_as */
							cmd_save_as(g_filename, dest);
						} else {
							cmd_save_as(g_filename, "~");
							/* no path => fallback to cmd_save */
						}
					} else if (strncmp(cmdline, "convert", 7) == 0) {
						char *dest = cmdline + 7;
						while (*dest == ' ' || *dest == '\t') {
							dest++;
						}
						cmd_convert(g_filename, dest);
					} else if (strncmp(cmdline, "delete", 6) == 0) {
						cmd_delete(g_filename);
					} else if (strncmp(cmdline, "bookmark", 8) == 0) {
						char *lbl = cmdline + 8;
						while (*lbl == ' ' || *lbl == '\t') lbl++;
						cmd_bookmark(g_filename, lbl, g_config);
					}

					/* reset */
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
				} else if (ks == XK_Tab) {
					do_tab_completion();
					render_image(dpy, win);
				} else {
					/* typed ASCII => append to command buffer if printable. */
					if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
						if (g_command_len < (int)(sizeof(g_command_input) - 1)) {
							g_command_input[g_command_len++] = buf[0];
							g_command_input[g_command_len] = '\0';
						}
					}
					render_image(dpy, win);
				}
			} else {
				/* not in command mode */
				if (len == 1 && buf[0] == ':') {
					g_command_mode = 1;
					g_command_len = 1;
					g_command_input[0] = ':';
					g_command_input[1] = '\0';
					render_image(dpy, win);
					break;
				}
				is_ctrl_pressed = (ev.xkey.state & ControlMask) != 0;
				switch (ks) {
				case XK_q:
					return;
				case XK_space:
					if (vdata->currentIndex < vdata->fileCount - 1) {
						vdata->currentIndex++;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					}
					break;
				case XK_BackSpace:
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
					/* Possibly a config-defined key binding. */
					break;
				}
			}
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
			/* Could do drag-based panning. */
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
		if (g_cmdFont) {
			/* XFreeFont(dpy, g_cmdFont); */
		}
		XCloseDisplay(dpy);
	}
}

