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

/* Command bar geometry / font */
#define CMD_BAR_HEIGHT 15
#define CMD_BAR_FONT   "monospace"

/* Gallery thumbnails: set a fixed size (scaled) */
#define THUMB_W 128
#define THUMB_H 128

#define GALLERY_COLS 4     /* number of thumbnails per row in gallery view */
#define GALLERY_BG   "#333333"  /* color behind thumbnails in gallery */

/* Zoom constraints */
#define ZOOM_STEP 0.1
#define MIN_ZOOM 0.1
#define MAX_ZOOM 20.0

/* We'll have two modes: normal and gallery. */
static int g_gallery_mode = 0;  /* 0 = normal, 1 = gallery */

/* The currently displayed wand, plus dimensions, pan/zoom, etc. */
static MagickWand *g_wand = NULL;
static int g_img_width = 0;
static int g_img_height = 0;
static int g_pan_x = 0;
static int g_pan_y = 0;
static double g_zoom = 1.0;

/* Current file name, pointer to config, etc. */
static char g_filename[1024] = {0};
static MsxivConfig *g_config = NULL;

/* For the command bar input */
static char g_command_input[1024] = {0};
static int g_command_mode = 0;
static int g_command_len = 0;

/* Colors/pixels for background, text, command bar background, gallery BG */
static unsigned long g_bg_pixel = 0;
static unsigned long g_text_pixel = 0;
static unsigned long g_cmdbar_bg_pixel = 0;
static unsigned long g_gallery_bg_pixel = 0;

/* Font for the command bar text */
static XFontStruct *g_cmdFont = NULL;

/* WM_DELETE_WINDOW for window close detection */
static Atom wmDeleteMessage;

/*
 * We'll store a small thumbnail XImage for each file in the ViewerData.
 * gallery_thumbs[i] corresponds to files[i].
 */
typedef struct {
	XImage *ximg;
	int w, h;  /* actual width/height of the thumbnail */
} GalleryThumb;

static GalleryThumb *g_thumbs = NULL;  /* array of length vdata->fileCount */
static int g_gallery_selection = 0;    /* which thumbnail is selected in gallery */

/* Forward declarations */
static void load_image(Display *dpy, Window win, const char *filename);
static void render_image(Display *dpy, Window win);
static void render_gallery(Display *dpy, Window win, ViewerData *vdata);

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
 * Load the main image from file into g_wand, reset zoom/pan, store name, then render.
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
 * Create a thumbnail XImage for a file: scale down to THUMB_W x THUMB_H (keeping aspect ratio).
 * Return an XImage pointer or NULL on failure.
 */
static XImage *create_thumbnail(Display *dpy, const char *filename, int *out_w, int *out_h)
{
	MagickWand *thumb_wand = NewMagickWand();
	if (MagickReadImage(thumb_wand, filename) == MagickFalse) {
		DestroyMagickWand(thumb_wand);
		return NULL;
	}
	int orig_w = (int)MagickGetImageWidth(thumb_wand);
	int orig_h = (int)MagickGetImageHeight(thumb_wand);

	/* scale to fit THUMB_W, THUMB_H, maintaining aspect ratio */
	double scale_x = (double)THUMB_W / (double)orig_w;
	double scale_y = (double)THUMB_H / (double)orig_h;
	double scale = (scale_x < scale_y) ? scale_x : scale_y;
	int new_w = (int)(orig_w * scale);
	int new_h = (int)(orig_h * scale);

	MagickResizeImage(thumb_wand, new_w, new_h, LanczosFilter);
	MagickSetImageFormat(thumb_wand, "RGBA");

	size_t length = 0;
	unsigned char *blob = MagickGetImageBlob(thumb_wand, &length);

	XImage *ximg = XCreateImage(dpy,
	                            DefaultVisual(dpy, DefaultScreen(dpy)),
	                            24, ZPixmap,
	                            0,
	                            (char *)malloc(new_w * new_h * 4),
	                            new_w, new_h,
	                            32, 0);
	if (!ximg) {
		MagickRelinquishMemory(blob);
		DestroyMagickWand(thumb_wand);
		return NULL;
	}
	memcpy(ximg->data, blob, new_w * new_h * 4);

	*out_w = new_w;
	*out_h = new_h;

	MagickRelinquishMemory(blob);
	DestroyMagickWand(thumb_wand);
	return ximg;
}

/*
 * Generate and store all thumbnails if multiple images are provided.
 * Called once in viewer_init if vdata->fileCount > 1.
 */
static void generate_gallery_thumbnails(Display *dpy, ViewerData *vdata)
{
	g_thumbs = calloc(vdata->fileCount, sizeof(GalleryThumb));
	if (!g_thumbs) {
		fprintf(stderr, "Failed to allocate gallery thumbs.\n");
		return;
	}
	for (int i = 0; i < vdata->fileCount; i++) {
		int tw = 0, th = 0;
		XImage *xi = create_thumbnail(dpy, vdata->files[i], &tw, &th);
		g_thumbs[i].ximg = xi;
		g_thumbs[i].w = tw;
		g_thumbs[i].h = th;
	}
}

/*
 * Render the normal image. If in command mode, draw the command bar.
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
	/* Clear background. */
	XSetForeground(dpy, gc, g_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	double scaled_w = g_img_width * g_zoom;
	double scaled_h = g_img_height * g_zoom;

	MagickWand *tmp = CloneMagickWand(g_wand);
	MagickResizeImage(tmp, (size_t)scaled_w, (size_t)scaled_h, LanczosFilter);

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
 * Render the gallery view: a grid of thumbnails with one selected.
 */
static void render_gallery(Display *dpy, Window win, ViewerData *vdata)
{
	if (!g_thumbs) {
		return; /* no thumbs => nothing to draw */
	}
	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	GC gc = DefaultGC(dpy, DefaultScreen(dpy));

	/* Fill background with gallery_bg_pixel. */
	XSetForeground(dpy, gc, g_gallery_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	int count = vdata->fileCount;
	int cols = GALLERY_COLS;
	int rows = (count + cols - 1) / cols;  /* ceiling of count/cols */

	/* We'll define some spacing around each thumbnail. */
	const int thumb_spacing_x = 10;
	const int thumb_spacing_y = 10;
	const int offset_x = 20; /* left margin */
	const int offset_y = 20; /* top margin */

	for (int i = 0; i < count; i++) {
		GalleryThumb *th = &g_thumbs[i];
		if (!th->ximg) {
			continue;
		}
		int row = i / cols;
		int col = i % cols;
		int x = offset_x + col * (THUMB_W + thumb_spacing_x);
		int y = offset_y + row * (THUMB_H + thumb_spacing_y);

		/* Center the thumbnail if it's smaller than THUMB_W/H. */
		int dx = (THUMB_W - th->w) / 2;
		int dy = (THUMB_H - th->h) / 2;

		XPutImage(dpy, win, gc, th->ximg,
		          0, 0,
		          x + dx, y + dy,
		          th->w, th->h);

		/* If this is the selected thumbnail, draw a highlight rectangle. */
		if (i == g_gallery_selection) {
			XSetForeground(dpy, gc, g_text_pixel); /* white highlight */
			XDrawRectangle(dpy, win, gc, x, y, THUMB_W, THUMB_H);
		}
	}
}

/*
 * viewer_init: open display, create window, load background color, 
 * generate thumbnails if multiple images, etc.
 */
int viewer_init(Display **dpy, Window *win, ViewerData *vdata, MsxivConfig *config)
{
	MagickWandGenesis();
	g_config = config;
	g_wand = NULL;
	g_gallery_mode = 0;
	g_thumbs = NULL;
	g_gallery_selection = 0;

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

	XEvent e;
	while (1) {
		XNextEvent(*dpy, &e);
		if (e.type == MapNotify) {
			break;
		}
	}

	/* Load background color from config->bg_color. */
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

	/* White text color. */
	g_text_pixel = WhitePixel(*dpy, screen);

	/* Command bar forced black background. */
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

	/* Gallery background color (#333333). */
	{
		Colormap cmap = DefaultColormap(*dpy, screen);
		XColor xcol;
		if (XParseColor(*dpy, cmap, GALLERY_BG, &xcol) &&
		    XAllocColor(*dpy, cmap, &xcol)) {
			g_gallery_bg_pixel = xcol.pixel;
		} else {
			g_gallery_bg_pixel = BlackPixel(*dpy, screen);
		}
	}

	/* Load the monospace font. */
	g_cmdFont = XLoadQueryFont(*dpy, CMD_BAR_FONT);
	if (!g_cmdFont) {
		g_cmdFont = XLoadQueryFont(*dpy, "fixed");
	}

	/* If multiple images => generate gallery thumbnails. */
	if (vdata->fileCount > 1) {
		generate_gallery_thumbnails(*dpy, vdata);
	}

	/* Load the first (currentIndex) image as normal. */
	if (vdata->fileCount > 0) {
		load_image(*dpy, *win, vdata->files[vdata->currentIndex]);
	}
	return 0;
}

/*
 * The main event loop, now with two modes:
 * - normal: show single image + command bar
 * - gallery: show thumbnails in a grid, arrow keys to move selection, enter to open
 */
void viewer_run(Display *dpy, Window win, ViewerData *vdata)
{
	XEvent ev;
	int is_ctrl_pressed = 0;

	g_command_input[0] = '\0';
	g_command_len = 0;
	g_gallery_mode = 0;

	while (1) {
		XNextEvent(dpy, &ev);

		switch (ev.type) {
		case Expose:
		case ConfigureNotify:
			if (g_gallery_mode) {
				render_gallery(dpy, win, vdata);
			} else {
				render_image(dpy, win);
			}
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

			if (g_gallery_mode) {
				/*
				 * GALLERY MODE:
				 * - arrows move selection
				 * - enter opens selected -> exit gallery
				 * - esc => exit gallery
				 */
				switch (ks) {
				case XK_Escape:
					g_gallery_mode = 0;
					render_image(dpy, win);
					break;
        case XK_Return:
        case XK_KP_Enter:
            if (g_gallery_selection >= 0 &&
                g_gallery_selection < vdata->fileCount)
            {
                g_gallery_mode = 0;
                /* Switch to the selected file */
                vdata->currentIndex = g_gallery_selection;
                /* Load the new image (which calls render_image internally) */
                load_image(dpy, win, vdata->files[vdata->currentIndex]);
            }
            render_image(dpy, win);
            XFlush(dpy);
            break;

				case XK_Left:
					if (g_gallery_selection % GALLERY_COLS != 0) {
						g_gallery_selection--;
					}
					break;
				case XK_Right:
					if (g_gallery_selection < vdata->fileCount - 1 &&
					    (g_gallery_selection % GALLERY_COLS != (GALLERY_COLS - 1))) {
						g_gallery_selection++;
					}
					break;
				case XK_Up:
					if (g_gallery_selection - GALLERY_COLS >= 0) {
						g_gallery_selection -= GALLERY_COLS;
					}
					break;
				case XK_Down:
					if (g_gallery_selection + GALLERY_COLS < vdata->fileCount) {
						g_gallery_selection += GALLERY_COLS;
					}
					break;
				default:
					break;
				}
        if (g_gallery_mode)
  				render_gallery(dpy, win, vdata);
			} else if (g_command_mode) {
				/*
				 * COMMAND MODE: typed commands.
				 */
				if (ks == XK_Return) {
					g_command_input[g_command_len] = '\0';
					g_command_mode = 0;
					/* parse command => e.g. :save, :convert, etc. */
					char *cmdline = g_command_input;
					if (cmdline[0] == ':') {
						cmdline++;
					}
					/* handle commands (save, convert, delete, bookmark)... 
					   same as your existing logic. 
					*/
					/* For brevity, omitted. */

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
					/* do tab completion if you have it, etc. */
					render_image(dpy, win);
				} else {
					if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
						if (g_command_len < (int)(sizeof(g_command_input) - 1)) {
							g_command_input[g_command_len++] = buf[0];
							g_command_input[g_command_len] = '\0';
						}
					}
					render_image(dpy, win);
				}
			} else {
				/*
				 * NORMAL MODE:
				 * arrow keys => pan
				 * space => next image
				 * backspace => previous image
				 * plus/minus => zoom, = => fit
				 * q => quit
				 * if multiple images => ENTER => toggle gallery
				 * colon => command mode
				 */
				is_ctrl_pressed = (ev.xkey.state & ControlMask) != 0;

				if (len == 1 && buf[0] == ':') {
					g_command_mode = 1;
					g_command_len = 1;
					g_command_input[0] = ':';
					g_command_input[1] = '\0';
					render_image(dpy, win);
					break;
				}

				switch (ks) {
				case XK_q:
					return;
				case XK_Return:
				case XK_KP_Enter:
					/*
					 * If multiple images, enter gallery mode.
					 */
					if (vdata->fileCount > 1) {
						g_gallery_mode = 1;
						g_gallery_selection = vdata->currentIndex;
						render_gallery(dpy, win, vdata);
					}
					break;
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
					/* Possibly a config-defined key binding, etc. */
					break;
				}
			}
		} break; /* end KeyPress */

		case ButtonPress:
			if (!g_gallery_mode) {
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
			}
			break;
		case ButtonRelease:
			break;
		case MotionNotify:
			break;
		}
	}
}

/*
 * Cleanup: free wand, free thumbnails, close display, etc.
 */
void viewer_cleanup(Display *dpy)
{
	if (g_wand) {
		DestroyMagickWand(g_wand);
		g_wand = NULL;
	}
	if (g_thumbs) {
		/* free each XImage used for thumbnails */
		/* We need the GC to free them properly, or just free the data pointer. */
		free(g_thumbs);
		g_thumbs = NULL;
	}
	MagickWandTerminus();

	if (dpy) {
		if (g_cmdFont) {
			/* XFreeFont(dpy, g_cmdFont); */
		}
		XCloseDisplay(dpy);
	}
}
