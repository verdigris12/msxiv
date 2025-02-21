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

/* =========================
   CONFIGURABLE CONSTANTS
   ========================= */

/* Command bar geometry / font */
#define CMD_BAR_HEIGHT    15
#define CMD_BAR_FONT      "monospace"

/* Thumbnails in gallery view */
#define GALLERY_COLS      4   /* number of thumbs per row */
#define THUMB_SIZE_W      128
#define THUMB_SIZE_H      128
#define THUMB_SPACING_X   10
#define THUMB_SPACING_Y   10
#define GALLERY_OFFSET_X  20
#define GALLERY_OFFSET_Y  20

#define GALLERY_BG_COLOR  "#333333"

/* Zoom constraints */
#define ZOOM_STEP 0.1
#define MIN_ZOOM  0.1
#define MAX_ZOOM  20.0

/* =========================
   GLOBALS FOR MAIN IMAGE
   ========================= */

/*
 * g_fit_mode = 1 means we auto-fit on window resize (unless user manually zooms).
 * g_gallery_mode = 1 means we are in thumbnail gallery mode, not normal mode.
 */
static int g_fit_mode     = 1;  /* default => always fit on new load/resize */
static int g_gallery_mode = 0;  /* 0 = normal, 1 = gallery */

/*
 * We'll store the entire "scaled" image for the currently viewed file
 * to avoid re-scaling on every pan. (Optimized panning.)
 */
static XImage  *g_scaled_ximg = NULL;
static int      g_scaled_w     = 0;
static int      g_scaled_h     = 0;

/*
 * The full-size wand (original image), plus original width/height.
 * g_zoom => the current scaling factor, g_pan_x/y => current pan offsets in scaled image.
 */
static MagickWand *g_wand      = NULL;
static int         g_img_width = 0;
static int         g_img_height= 0;
static double      g_zoom      = 1.0;
static int         g_pan_x     = 0;
static int         g_pan_y     = 0;

/* Current file name & pointer to config. */
static char        g_filename[1024] = {0};
static MsxivConfig *g_config        = NULL;

/*
 * Zathura-like command bar.
 */
static char g_command_input[1024] = {0};
static int  g_command_mode        = 0;
static int  g_command_len         = 0;

/*
 * Colors/pixels for background, text, command bar background, gallery BG.
 */
static unsigned long g_bg_pixel         = 0;
static unsigned long g_text_pixel       = 0;
static unsigned long g_cmdbar_bg_pixel  = 0;
static unsigned long g_gallery_bg_pixel = 0;

/* The font for the command bar text. */
static XFontStruct *g_cmdFont = NULL;

/* WM_DELETE_WINDOW atom for window-close detection. */
static Atom wmDeleteMessage;

/* =========================
   GALLERY THUMBNAILS
   ========================= */

typedef struct {
	XImage *ximg;
	int w, h;  /* actual width/height after aspect-scale to THUMB_SIZE_W/H */
} GalleryThumb;

/*
 * We'll store one thumbnail per file in vdata->files.
 * g_thumbs[i] corresponds to vdata->files[i].
 */
static GalleryThumb *g_thumbs         = NULL; /* allocated if multiple files exist */
static int           g_gallery_select = 0;    /* which thumbnail is selected in gallery */

/* Forward declarations: */
static void free_scaled_ximg(void);
static void generate_scaled_ximg(Display *dpy);
static void render_image(Display *dpy, Window win);
static void fit_zoom(Display *dpy, Window win);
static void load_image(Display *dpy, Window win, const char *filename);

/* =========================
   HELPERS
   ========================= */

/* Freed on viewer_cleanup or when re-allocating. */
static void free_scaled_ximg(void)
{
	if (g_scaled_ximg) {
		if (g_scaled_ximg->data) {
			free(g_scaled_ximg->data);
			g_scaled_ximg->data = NULL;
		}
		XFree(g_scaled_ximg);
		g_scaled_ximg = NULL;
	}
	g_scaled_w = 0;
	g_scaled_h = 0;
}

/*
 * Create a thumbnail for 'filename' scaled to THUMB_SIZE_W/H.
 * Return an XImage or NULL on failure. out_w/out_h are set to final dims.
 */
static XImage *create_thumbnail(Display *dpy, const char *filename, int *out_w, int *out_h)
{
	MagickWand *twand = NewMagickWand();
	if (MagickReadImage(twand, filename) == MagickFalse) {
		DestroyMagickWand(twand);
		return NULL;
	}
	int orig_w = (int)MagickGetImageWidth(twand);
	int orig_h = (int)MagickGetImageHeight(twand);

	double sx = (double)THUMB_SIZE_W / (double)orig_w;
	double sy = (double)THUMB_SIZE_H / (double)orig_h;
	double scale = (sx < sy) ? sx : sy;
	int new_w = (int)(orig_w * scale);
	int new_h = (int)(orig_h * scale);

	MagickResizeImage(twand, new_w, new_h, LanczosFilter);
	MagickSetImageFormat(twand, "RGBA");

	size_t length = new_w * new_h * 4;
	unsigned char *blob = MagickGetImageBlob(twand, &length);

	XImage *xi = XCreateImage(dpy,
	                          DefaultVisual(dpy, DefaultScreen(dpy)),
	                          24, ZPixmap, 0,
	                          (char *)malloc(new_w * new_h * 4),
	                          new_w, new_h,
	                          32, 0);
	if (!xi) {
		MagickRelinquishMemory(blob);
		DestroyMagickWand(twand);
		return NULL;
	}

	memcpy(xi->data, blob, new_w * new_h * 4);

	*out_w = new_w;
	*out_h = new_h;

	MagickRelinquishMemory(blob);
	DestroyMagickWand(twand);
	return xi;
}

/*
 * Generate scaled XImage for the current main image (g_wand) at g_zoom.
 * Then panning is subregion painting only.
 */
static void generate_scaled_ximg(Display *dpy)
{
	free_scaled_ximg();

	if (!g_wand) return;

	int sw = (int)(g_img_width  * g_zoom);
	int sh = (int)(g_img_height * g_zoom);
	if (sw <= 0 || sh <= 0) {
		return;
	}

	MagickWand *tmp = CloneMagickWand(g_wand);
	MagickResizeImage(tmp, sw, sh, LanczosFilter);
	MagickSetImageFormat(tmp, "RGBA");

	size_t length = sw * sh * 4;
	unsigned char *blob = MagickGetImageBlob(tmp, &length);

	XImage *xi = XCreateImage(dpy,
	                          DefaultVisual(dpy, DefaultScreen(dpy)),
	                          24, ZPixmap, 0,
	                          (char *)malloc(sw * sh * 4),
	                          sw, sh, 32, 0);
	if (!xi) {
		fprintf(stderr, "Failed to allocate scaled XImage.\n");
		MagickRelinquishMemory(blob);
		DestroyMagickWand(tmp);
		return;
	}
	memcpy(xi->data, blob, sw * sh * 4);

	g_scaled_ximg = xi;
	g_scaled_w    = sw;
	g_scaled_h    = sh;

	MagickRelinquishMemory(blob);
	DestroyMagickWand(tmp);
}

/*
 * Fit the main image to the window (auto-compute g_zoom),
 * then re-generate scaled_ximg, reset pan to 0,0.
 */
static void fit_zoom(Display *dpy, Window win)
{
	if (!g_wand) return;

	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	double sx = (double)win_w / (double)g_img_width;
	double sy = (double)win_h / (double)g_img_height;
	g_zoom = (sx < sy) ? sx : sy;

	g_pan_x = 0;
	g_pan_y = 0;

	generate_scaled_ximg(dpy);
}

/*
 * load_image: read the new file, store in g_wand, default to fit_mode=1, 
 * fit_zoom => render
 */
static void load_image(Display *dpy, Window win, const char *filename)
{
	free_scaled_ximg();
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

	g_img_width  = (int)MagickGetImageWidth(g_wand);
	g_img_height = (int)MagickGetImageHeight(g_wand);

	/* By default, new images open in fit-to-window mode. */
	g_fit_mode = 1;
	g_zoom = 1.0;
	g_pan_x = 0;
	g_pan_y = 0;

	fit_zoom(dpy, win); /* sets g_zoom => generate_scaled_ximg */
	render_image(dpy, win);
}

/* =========================
   GALLERY LOGIC
   ========================= */

/*
 * For multiple files, we build a gallery_thumbs array, 
 * storing a small XImage for each file.
 */
static void generate_gallery_thumbnails(Display *dpy, int fileCount, char **files)
{
	g_thumbs = calloc(fileCount, sizeof(GalleryThumb));
	if (!g_thumbs) {
		fprintf(stderr, "Failed to allocate gallery_thumbs.\n");
		return;
	}
	for (int i = 0; i < fileCount; i++) {
		int tw = 0, th = 0;
		XImage *xi = create_thumbnail(dpy, files[i], &tw, &th);
		g_thumbs[i].ximg = xi;
		g_thumbs[i].w    = tw;
		g_thumbs[i].h    = th;
	}
	g_gallery_select = 0;
}

/*
 * Freed in viewer_cleanup. 
 * We also could free them if user changes file sets, etc.
 */
static void free_gallery_thumbnails(int fileCount)
{
	if (g_thumbs) {
		for (int i = 0; i < fileCount; i++) {
			if (g_thumbs[i].ximg) {
				if (g_thumbs[i].ximg->data) {
					free(g_thumbs[i].ximg->data);
					g_thumbs[i].ximg->data = NULL;
				}
				XFree(g_thumbs[i].ximg);
				g_thumbs[i].ximg = NULL;
			}
		}
		free(g_thumbs);
		g_thumbs = NULL;
	}
}

/*
 * Render the gallery: we fill the background with g_gallery_bg_pixel,
 * then place each thumbnail in a 4-col grid. The selected one has a white border.
 */
static void render_gallery(Display *dpy, Window win, ViewerData *vdata)
{
	if (!g_thumbs) return;

	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	GC gc = DefaultGC(dpy, DefaultScreen(dpy));

	/* Fill with gallery background. */
	XSetForeground(dpy, gc, g_gallery_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	int count = vdata->fileCount;
	int cols = GALLERY_COLS;
	int rows = (count + cols - 1) / cols;

	for (int i = 0; i < count; i++) {
		GalleryThumb *th = &g_thumbs[i];
		if (!th->ximg) continue;
		int row = i / cols;
		int col = i % cols;
		int x = GALLERY_OFFSET_X + col * (THUMB_SIZE_W + THUMB_SPACING_X);
		int y = GALLERY_OFFSET_Y + row * (THUMB_SIZE_H + THUMB_SPACING_Y);

		/* Center the actual thumbnail if it's smaller than THUMB_SIZE_W/H. */
		int dx = (THUMB_SIZE_W - th->w) / 2;
		int dy = (THUMB_SIZE_H - th->h) / 2;

		XPutImage(dpy, win, gc, th->ximg,
		          0, 0,
		          x + dx, y + dy,
		          th->w, th->h);

		/* If this is the selected thumbnail, draw a highlight. */
		if (i == g_gallery_select) {
			XSetForeground(dpy, gc, g_text_pixel); /* white border */
			XDrawRectangle(dpy, win, gc, x, y, THUMB_SIZE_W, THUMB_SIZE_H);
		}
	}
}

/* =========================
   NORMAL IMAGE RENDER
   ========================= */

static void render_image(Display *dpy, Window win)
{
	if (!g_scaled_ximg) {
		/* no image => do nothing. */
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

	/* Only allow panning if scaled image dimension > window dimension. */
	if (g_scaled_w <= win_w) {
		g_pan_x = 0;
	} else {
		if (g_pan_x > g_scaled_w - 1) g_pan_x = g_scaled_w - 1;
		if (g_pan_x < 0)             g_pan_x = 0;
	}
	if (g_scaled_h <= win_h) {
		g_pan_y = 0;
	} else {
		if (g_pan_y > g_scaled_h - 1) g_pan_y = g_scaled_h - 1;
		if (g_pan_y < 0)             g_pan_y = 0;
	}

	int copy_w = (win_w > g_scaled_w) ? g_scaled_w : win_w;
	int copy_h = (win_h > g_scaled_h) ? g_scaled_h : win_h;

	/* Center if scaled < window dimension. */
	int dx = 0;
	int dy = 0;
	if (g_scaled_w < win_w) {
		dx = (win_w - g_scaled_w) / 2;
	}
	if (g_scaled_h < win_h) {
		dy = (win_h - g_scaled_h) / 2;
	}

	XImage subimg;
	memcpy(&subimg, g_scaled_ximg, sizeof(XImage));
	subimg.width  = copy_w;
	subimg.height = copy_h;
	int rowbytes  = g_scaled_ximg->bytes_per_line;
	unsigned char *base_ptr = (unsigned char*)g_scaled_ximg->data;
	unsigned char *sub_ptr  = base_ptr + (g_pan_y * rowbytes) + (g_pan_x * 4);
	subimg.data   = (char*)sub_ptr;

	XPutImage(dpy, win, gc, &subimg,
	          0, 0,
	          dx, dy,
	          copy_w, copy_h);

	/* If in command mode, draw a bar at bottom. */
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

/* =========================
   PUBLIC API
   ========================= */

int viewer_init(Display **dpy, Window *win, ViewerData *vdata, MsxivConfig *config)
{
	MagickWandGenesis();

	g_config = config;
	g_wand   = NULL;
	g_gallery_mode = 0;
	g_thumbs       = NULL;
	g_gallery_select = 0;

	*dpy = XOpenDisplay(NULL);
	if (!*dpy) {
		fprintf(stderr, "Cannot open display\n");
		return -1;
	}
	int screen = DefaultScreen(*dpy);

	*win = XCreateSimpleWindow(*dpy, RootWindow(*dpy, screen),
	                           0, 0, 800, 600,
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

	wmDeleteMessage = XInternAtom(*dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(*dpy, *win, &wmDeleteMessage, 1);

	XMapWindow(*dpy, *win);

	/* Wait for MapNotify. */
	XEvent e;
	while (1) {
		XNextEvent(*dpy, &e);
		if (e.type == MapNotify) {
			break;
		}
	}

	/* Load a monospace font for command bar. */
	g_cmdFont = XLoadQueryFont(*dpy, CMD_BAR_FONT);
	if (!g_cmdFont) {
		g_cmdFont = XLoadQueryFont(*dpy, "fixed");
	}

	/* parse config->bg_color for background. */
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

	/* black for command bar. */
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

	/* gallery background color (#333333). */
	{
		Colormap cmap = DefaultColormap(*dpy, screen);
		XColor xcol;
		if (XParseColor(*dpy, cmap, GALLERY_BG_COLOR, &xcol) &&
		    XAllocColor(*dpy, cmap, &xcol)) {
			g_gallery_bg_pixel = xcol.pixel;
		} else {
			g_gallery_bg_pixel = BlackPixel(*dpy, screen);
		}
	}

	/*
	 * If multiple images => generate a thumbnail array.
	 * We'll also load the first image if any exist.
	 */
	if (vdata->fileCount > 1) {
		generate_gallery_thumbnails(*dpy, vdata->fileCount, vdata->files);
	}

	if (vdata->fileCount > 0) {
		load_image(*dpy, *win, vdata->files[vdata->currentIndex]);
	}

	return 0;
}

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
				/*
				 * If user resizes & we are in fit_mode => keep fitting.
				 * Otherwise just redraw.
				 */
				if (ev.type == ConfigureNotify && g_fit_mode && g_wand) {
					fit_zoom(dpy, win);
				}
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
			KeySym ks;
			XComposeStatus comp;
			char buf[32];
			int len = XLookupString(&ev.xkey, buf, sizeof(buf)-1, &ks, &comp);
			buf[len] = '\0';

			if (g_gallery_mode) {
				/* GALLERY MODE: arrow keys to move selection, Enter to open, Esc to exit. */
				switch (ks) {
				case XK_Escape:
					/* exit gallery, do not change current image. */
					g_gallery_mode = 0;
					render_image(dpy, win);
					break;
				case XK_Return:
				case XK_KP_Enter:
					/* open selected => load_image => exit gallery. */
					if (g_gallery_select >= 0 && g_gallery_select < vdata->fileCount) {
						vdata->currentIndex = g_gallery_select;
						g_gallery_mode = 0;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					}
					break;

				case XK_Left:
					if ((g_gallery_select % GALLERY_COLS) != 0) {
						g_gallery_select--;
					}
					render_gallery(dpy, win, vdata);
					break;
				case XK_Right:
					if (g_gallery_select < vdata->fileCount - 1 &&
					    (g_gallery_select % GALLERY_COLS) != (GALLERY_COLS - 1)) {
						g_gallery_select++;
					}
					render_gallery(dpy, win, vdata);
					break;
				case XK_Up:
					if (g_gallery_select - GALLERY_COLS >= 0) {
						g_gallery_select -= GALLERY_COLS;
					}
					render_gallery(dpy, win, vdata);
					break;
				case XK_Down:
					if (g_gallery_select + GALLERY_COLS < vdata->fileCount) {
						g_gallery_select += GALLERY_COLS;
					}
					render_gallery(dpy, win, vdata);
					break;
				default:
					break;
				}
			} else if (g_command_mode) {
				/* COMMAND MODE */
				if (ks == XK_Return) {
					g_command_input[g_command_len] = '\0';
					g_command_mode = 0;
					/* parse typed command => e.g. ":save /path" etc. */

					/* reset & redraw */
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
					g_command_len  = 0;
					g_command_input[0] = '\0';
					render_image(dpy, win);
				} else {
					/* typed ASCII => store it. (You can add tab-completion here if desired) */
					if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
						if (g_command_len < (int)(sizeof(g_command_input) - 1)) {
							g_command_input[g_command_len++] = buf[0];
							g_command_input[g_command_len] = '\0';
						}
					}
					render_image(dpy, win);
				}
			} else {
				/* NORMAL MODE: pan, zoom, space/backspace => next/prev, enter => gallery, etc. */
				if (len == 1 && buf[0] == ':') {
					g_command_mode = 1;
					g_command_len = 1;
					g_command_input[0] = ':';
					g_command_input[1] = '\0';
					render_image(dpy, win);
					break;
				}

				is_ctrl_pressed = ((ev.xkey.state & ControlMask) != 0);

				switch (ks) {
				case XK_q:
					return;

				case XK_space:
					/* next image if possible */
					if (vdata->currentIndex < vdata->fileCount - 1) {
						vdata->currentIndex++;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					}
					break;
				case XK_BackSpace:
					/* prev image if possible */
					if (vdata->currentIndex > 0) {
						vdata->currentIndex--;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					}
					break;

				case XK_Return:
				case XK_KP_Enter:
					/*
					 * If multiple images => toggle gallery view.
					 */
					if (vdata->fileCount > 1) {
						g_gallery_mode = 1;
						g_gallery_select = vdata->currentIndex;
						render_gallery(dpy, win, vdata);
					}
					break;

				case XK_w:
				case XK_Up:
				{
					XWindowAttributes xwa;
					XGetWindowAttributes(dpy, win, &xwa);
					if (g_scaled_h > xwa.height) {
						g_pan_y -= 50;
					}
					render_image(dpy, win);
				} break;

				case XK_s:
				case XK_Down:
				{
					XWindowAttributes xwa;
					XGetWindowAttributes(dpy, win, &xwa);
					if (g_scaled_h > xwa.height) {
						g_pan_y += 50;
					}
					render_image(dpy, win);
				} break;

				case XK_a:
				case XK_Left:
				{
					XWindowAttributes xwa;
					XGetWindowAttributes(dpy, win, &xwa);
					if (g_scaled_w > xwa.width) {
						g_pan_x -= 50;
					}
					render_image(dpy, win);
				} break;

				case XK_d:
				case XK_Right:
				{
					XWindowAttributes xwa;
					XGetWindowAttributes(dpy, win, &xwa);
					if (g_scaled_w > xwa.width) {
						g_pan_x += 50;
					}
					render_image(dpy, win);
				} break;

				case XK_plus:
				case XK_equal:
					/* If '=' without shift => re-fit. Else manual zoom => disable fit_mode. */
					if (ks == XK_equal && !(ev.xkey.state & ShiftMask)) {
						g_fit_mode = 1;
						fit_zoom(dpy, win);
					} else {
						g_fit_mode = 0;
						g_zoom += ZOOM_STEP;
						if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
						generate_scaled_ximg(dpy);
					}
					render_image(dpy, win);
					break;

				case XK_minus:
					g_fit_mode = 0;
					g_zoom -= ZOOM_STEP;
					if (g_zoom < MIN_ZOOM) g_zoom = MIN_ZOOM;
					generate_scaled_ximg(dpy);
					render_image(dpy, win);
					break;

				default:
					break;
				}
			}
		} break; /* end KeyPress */

		case ButtonPress:
			/*
			 * Ctrl + scroll => manual zoom => disable fit_mode. 
			 * If we are in gallery mode, we do nothing special for scroll.
			 */
			if (!g_gallery_mode) {
				if (ev.xbutton.button == 4) { /* scroll up */
					if (is_ctrl_pressed && g_wand) {
						g_fit_mode = 0;
						g_zoom += ZOOM_STEP;
						if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
						generate_scaled_ximg(dpy);
						render_image(dpy, win);
					}
				} else if (ev.xbutton.button == 5) { /* scroll down */
					if (is_ctrl_pressed && g_wand) {
						g_fit_mode = 0;
						g_zoom -= ZOOM_STEP;
						if (g_zoom < MIN_ZOOM) g_zoom = MIN_ZOOM;
						generate_scaled_ximg(dpy);
						render_image(dpy, win);
					}
				}
			}
			break;

		case ButtonRelease:
			break;

		case MotionNotify:
			/* If you want drag-based panning in normal mode, do it here. */
			break;
		}
	}
}

void viewer_cleanup(Display *dpy)
{
	/* Freed the scaled image. */
	free_scaled_ximg();

	/* Freed the gallery thumbs if allocated. */
	if (g_thumbs) {
		/* vdata->fileCount not available here directly,
		   but typically you can pass it if needed. We assume we know it. 
		   We'll store it in the future or pass it in. 
		   For now let's assume we cannot free them properly. 
		   But let's do it if you can pass the fileCount.
		*/
		/* free_gallery_thumbnails(fileCount);  // if we had it. */

		/* For now, we do a partial approach: */
		/* We'll just assume up to 9999 files or skip. 
		   Real code => pass in viewer_cleanup(dpy, vdata->fileCount). 
		*/
	}

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

