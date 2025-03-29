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

/*
 * =========================
 * CONFIGURABLE CONSTANTS
 * =========================
 */

#define CMD_BAR_HEIGHT    15
#define CMD_BAR_FONT      "monospace"

#define GALLERY_COLS      4
#define THUMB_SIZE_W      128
#define THUMB_SIZE_H      128
#define THUMB_SPACING_X   10
#define THUMB_SPACING_Y   10
#define GALLERY_OFFSET_X  20
#define GALLERY_OFFSET_Y  20

#define GALLERY_BG_COLOR  "#333333"

#define ZOOM_STEP 0.1
#define MIN_ZOOM  0.1
#define MAX_ZOOM  20.0

/*
 * =========================
 * GLOBALS FOR MAIN IMAGE
 * =========================
 */

static int g_fit_mode     = 1;
static int g_gallery_mode = 0;

static XImage     *g_scaled_ximg = NULL;
static int         g_scaled_w     = 0;
static int         g_scaled_h     = 0;
static MagickWand *g_wand        = NULL;
static int         g_img_width   = 0;
static int         g_img_height  = 0;
static double      g_zoom        = 1.0;
static int         g_pan_x       = 0;
static int         g_pan_y       = 0;

static char        g_filename[1024] = {0};
static MsxivConfig *g_config        = NULL;

static char g_command_input[1024] = {0};
static int  g_command_mode        = 0;
static int  g_command_len         = 0;

static unsigned long g_bg_pixel         = 0;
static unsigned long g_text_pixel       = 0;
static unsigned long g_cmdbar_bg_pixel  = 0;
static unsigned long g_gallery_bg_pixel = 0;

static XFontStruct *g_cmdFont = NULL;
static Atom         wmDeleteMessage;

/* Thumbnails for gallery mode */
typedef struct {
	XImage *ximg;
	int w;
	int h;
} GalleryThumb;

static GalleryThumb *g_thumbs         = NULL;
static int           g_gallery_select = 0;

/*
 * ============================================================
 * Minimal tab-completion logic (unchanged from previous):
 *  - Commands: "save", "save_as", "convert", "delete", "bookmark".
 *  - Path completion with ~, ., ..
 *  - If multiple matches => fill largest common prefix
 *  - If exactly one => fill fully + space
 * ============================================================
 */

static const char *g_known_cmds[] = {
	"save",
	"save_as",
	"convert",
	"delete",
	"bookmark",
	NULL
};

static void find_largest_common_prefix(char **list, int count, char *out_prefix, size_t out_sz)
{
	if (count <= 0) {
		out_prefix[0] = '\0';
		return;
	}
	if (count == 1) {
		snprintf(out_prefix, out_sz, "%s", list[0]);
		return;
	}
	snprintf(out_prefix, out_sz, "%s", list[0]);
	for (int i = 1; i < count; i++) {
		size_t j = 0;
		const char *other = list[i];
		while (out_prefix[j] && other[j] && out_prefix[j] == other[j]) {
			j++;
		}
		out_prefix[j] = '\0';
		if (j == 0) break;
	}
}

static void parse_path_prefix(const char *pathprefix,
                              char *dirbuf, size_t dirbuf_sz,
                              char *leafbuf, size_t leafbuf_sz)
{
	dirbuf[0]  = '\0';
	leafbuf[0] = '\0';
	if (!pathprefix || !*pathprefix) {
		snprintf(dirbuf, dirbuf_sz, ".");
		return;
	}
	if (pathprefix[0] == '~') {
		const char *home = getenv("HOME");
		if (!home) home = ".";
		if (pathprefix[1] == '/' || pathprefix[1] == '\0') {
			char sub[1024];
			snprintf(sub, sizeof(sub), "%s",
			         (pathprefix[1] == '/') ? (pathprefix + 2) : (pathprefix + 1));
			char *slash = strrchr(sub, '/');
			if (!slash) {
				snprintf(dirbuf, dirbuf_sz, "%s", home);
				snprintf(leafbuf, leafbuf_sz, "%s", sub);
			} else {
				*slash = '\0';
				snprintf(dirbuf, dirbuf_sz, "%s/%s", home, sub);
				snprintf(leafbuf, leafbuf_sz, "%s", slash + 1);
			}
		} else {
			snprintf(dirbuf, dirbuf_sz, ".");
			snprintf(leafbuf, leafbuf_sz, "%s", pathprefix);
		}
		return;
	}

	char copy[1024];
	snprintf(copy, sizeof(copy), "%s", pathprefix);
	char *slash = strrchr(copy, '/');
	if (!slash) {
		if (copy[0] == '/') {
			snprintf(dirbuf, dirbuf_sz, "/");
			snprintf(leafbuf, leafbuf_sz, "%s", copy + 1);
		} else {
			snprintf(dirbuf, dirbuf_sz, ".");
			snprintf(leafbuf, leafbuf_sz, "%s", copy);
		}
	} else {
		*slash = '\0';
		snprintf(dirbuf, dirbuf_sz, "%s", (copy[0] ? copy : "/"));
		snprintf(leafbuf, leafbuf_sz, "%s", slash + 1);
	}
}

static int gather_path_matches(const char *directory, const char *leaf,
                               char **list, int max_list)
{
	DIR *dirp = opendir(directory);
	if (!dirp) return 0;
	int count = 0;
	struct dirent *de;
	while ((de = readdir(dirp)) != NULL) {
		/* skip unless matches prefix */
		if (strncmp(de->d_name, leaf, strlen(leaf)) == 0) {
			if (count < max_list) {
				list[count] = strdup(de->d_name);
				if (list[count]) {
					count++;
				}
			} else break;
		}
	}
	closedir(dirp);
	return count;
}

static void free_string_list(char **list, int count)
{
	for (int i = 0; i < count; i++) {
		free(list[i]);
	}
}

static int is_command_in_list(const char *str, const char *list[])
{
	for (int i = 0; list[i]; i++) {
		if (strcmp(str, list[i]) == 0) return 1;
	}
	return 0;
}

static void try_tab_completion(void)
{
	if (!g_command_mode || g_command_len <= 0) return;
	if (g_command_input[0] != ':') return;

	char *space = strchr(g_command_input, ' ');
	if (!space) {
		/* Command completion */
		const char *prefix = g_command_input + 1;
		if (!*prefix) return;
		const char *matches[32];
		int match_count = 0;
		for (int i = 0; g_known_cmds[i]; i++) {
			if (!strncmp(prefix, g_known_cmds[i], strlen(prefix))) {
				if (match_count < 31) {
					matches[match_count++] = g_known_cmds[i];
				}
			}
		}
		if (match_count == 0) {
			return;
		} else if (match_count == 1) {
			char buf[1024];
			snprintf(buf, sizeof(buf), ":%s ", matches[0]);
			strncpy(g_command_input, buf, sizeof(g_command_input));
			g_command_input[sizeof(g_command_input) - 1] = '\0';
			g_command_len = strlen(g_command_input);
		} else {
			char prefixbuf[256];
			char *tmp_list[32];
			for (int i = 0; i < match_count; i++) {
				tmp_list[i] = (char*)matches[i];
			}
			find_largest_common_prefix(tmp_list, match_count,
			                           prefixbuf, sizeof(prefixbuf));
			if (strlen(prefixbuf) > strlen(prefix)) {
				char buf[1024];
				snprintf(buf, sizeof(buf), ":%s", prefixbuf);
				strncpy(g_command_input, buf, sizeof(g_command_input));
				g_command_input[sizeof(g_command_input) - 1] = '\0';
				g_command_len = strlen(g_command_input);
			}
		}
	} else {
		/* Path completion */
		char command[64];
		size_t len = space - (g_command_input + 1);
		if (len >= sizeof(command)) len = sizeof(command) - 1;
		memcpy(command, g_command_input + 1, len);
		command[len] = '\0';

		if (!is_command_in_list(command, g_known_cmds)) {
			return;
		}

		const char *pathprefix = space + 1;
		char dirbuf[1024], leafbuf[1024];
		parse_path_prefix(pathprefix, dirbuf, sizeof(dirbuf),
		                  leafbuf, sizeof(leafbuf));
		char *matches[256];
		int mcount = gather_path_matches(dirbuf, leafbuf, matches, 256);
		if (mcount == 0) {
			return;
		}
		if (mcount == 1) {
			char newleaf[1024];
			snprintf(newleaf, sizeof(newleaf), "%s", matches[0]);
			char combined[1024];
			if (!strcmp(dirbuf, ".")) {
				snprintf(combined, sizeof(combined), "%s", newleaf);
			} else if (!strcmp(dirbuf, "/")) {
				snprintf(combined, sizeof(combined), "/%s", newleaf);
			} else {
				snprintf(combined, sizeof(combined), "%s/%s", dirbuf, newleaf);
			}
			char buf[2048];
			snprintf(buf, sizeof(buf), ":%s %s ", command, combined);
			strncpy(g_command_input, buf, sizeof(g_command_input));
			g_command_input[sizeof(g_command_input) - 1] = '\0';
			g_command_len = strlen(g_command_input);
		} else {
			char *tmp_list[256];
			for (int i = 0; i < mcount; i++) {
				tmp_list[i] = matches[i];
			}
			char lcp[1024];
			find_largest_common_prefix(tmp_list, mcount, lcp, sizeof(lcp));
			if (strlen(lcp) > strlen(leafbuf)) {
				char combined[1024];
				if (!strcmp(dirbuf, ".")) {
					snprintf(combined, sizeof(combined), "%s", lcp);
				} else if (!strcmp(dirbuf, "/")) {
					snprintf(combined, sizeof(combined), "/%s", lcp);
				} else {
					snprintf(combined, sizeof(combined), "%s/%s", dirbuf, lcp);
				}
				char buf[2048];
				snprintf(buf, sizeof(buf), ":%s %s", command, combined);
				strncpy(g_command_input, buf, sizeof(g_command_input));
				g_command_input[sizeof(g_command_input) - 1] = '\0';
				g_command_len = strlen(g_command_input);
			}
		}
		free_string_list(matches, mcount);
	}
}

/*
 * =========================
 * FORWARD DECLARATIONS
 * =========================
 */

static void free_scaled_ximg(void);
static void generate_scaled_ximg(Display *dpy);
static void render_image(Display *dpy, Window win);
static void fit_zoom(Display *dpy, Window win);
static void load_image(Display *dpy, Window win, const char *filename);
static void free_gallery_thumbnails(int fileCount);

/*
 * =========================
 * THUMBNAIL GALLERY
 * =========================
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

static void free_gallery_thumbnails(int fileCount)
{
	if (!g_thumbs) return;
	for (int i = 0; i < fileCount; i++) {
		if (g_thumbs[i].ximg) {
			if (g_thumbs[i].ximg->data) {
				free(g_thumbs[i].ximg->data);
				g_thumbs[i].ximg->data = NULL;
			}
			XFree(g_thumbs[i].ximg);
		}
	}
	free(g_thumbs);
	g_thumbs = NULL;
}

static void render_gallery(Display *dpy, Window win, ViewerData *vdata)
{
	if (!g_thumbs) return;

	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	GC gc = DefaultGC(dpy, DefaultScreen(dpy));

	XSetForeground(dpy, gc, g_gallery_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	int count = vdata->fileCount;
	int cols  = GALLERY_COLS;
	int rows  = (count + cols - 1) / cols;

	for (int i = 0; i < count; i++) {
		GalleryThumb *th = &g_thumbs[i];
		if (!th->ximg) continue;
		int row = i / cols;
		int col = i % cols;
		int x = GALLERY_OFFSET_X + col * (THUMB_SIZE_W + THUMB_SPACING_X);
		int y = GALLERY_OFFSET_Y + row * (THUMB_SIZE_H + THUMB_SPACING_Y);

		int dx = (THUMB_SIZE_W - th->w) / 2;
		int dy = (THUMB_SIZE_H - th->h) / 2;

		XPutImage(dpy, win, gc, th->ximg, 0, 0, x + dx, y + dy, th->w, th->h);

		if (i == g_gallery_select) {
			XSetForeground(dpy, gc, g_text_pixel);
			XDrawRectangle(dpy, win, gc, x, y, THUMB_SIZE_W, THUMB_SIZE_H);
		}
	}
}

/*
 * =========================
 * IMAGE RENDER & LOAD
 * =========================
 */

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

static void generate_scaled_ximg(Display *dpy)
{
	free_scaled_ximg();
	if (!g_wand) return;

	int sw = (int)(g_img_width  * g_zoom);
	int sh = (int)(g_img_height * g_zoom);
	if (sw <= 0 || sh <= 0) return;

	MagickWand *tmp = CloneMagickWand(g_wand);
	MagickResizeImage(tmp, sw, sh, LanczosFilter);
	MagickSetImageFormat(tmp, "RGBA");

	size_t length = sw * sh * 4;
	unsigned char *blob = MagickGetImageBlob(tmp, &length);

	XImage *xi = XCreateImage(dpy,
	                          DefaultVisual(dpy, DefaultScreen(dpy)),
	                          24, ZPixmap, 0,
	                          (char *)malloc(sw * sh * 4),
	                          sw, sh,
	                          32, 0);
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

	g_fit_mode = 1;
	g_zoom = 1.0;
	g_pan_x = 0;
	g_pan_y = 0;

	fit_zoom(dpy, win);
}

static void render_image(Display *dpy, Window win)
{
	if (!g_scaled_ximg) return;

	XWindowAttributes xwa;
	XGetWindowAttributes(dpy, win, &xwa);
	int win_w = xwa.width;
	int win_h = xwa.height;

	GC gc = DefaultGC(dpy, DefaultScreen(dpy));

	XSetForeground(dpy, gc, g_bg_pixel);
	XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

	int copy_w = (g_scaled_w < win_w) ? g_scaled_w : win_w;
	int copy_h = (g_scaled_h < win_h) ? g_scaled_h : win_h;

	if (g_scaled_w <= win_w) {
		g_pan_x = 0;
	} else {
		if (g_pan_x < 0) {
			g_pan_x = 0;
		} else if (g_pan_x > g_scaled_w - copy_w) {
			g_pan_x = g_scaled_w - copy_w;
		}
	}

	if (g_scaled_h <= win_h) {
		g_pan_y = 0;
	} else {
		if (g_pan_y < 0) {
			g_pan_y = 0;
		} else if (g_pan_y > g_scaled_h - copy_h) {
			g_pan_y = g_scaled_h - copy_h;
		}
	}

	int dx = 0;
	int dy = 0;
	if (g_scaled_w < win_w) {
		dx = (win_w - g_scaled_w) / 2;
	}
	if (g_scaled_h < win_h) {
		dy = (win_h - g_scaled_h) / 2;
	}

	XImage sub_ximg;
	memcpy(&sub_ximg, g_scaled_ximg, sizeof(XImage));
	sub_ximg.width  = copy_w;
	sub_ximg.height = copy_h;

	int rowbytes = g_scaled_ximg->bytes_per_line;
	unsigned char *base_ptr = (unsigned char*)g_scaled_ximg->data;
	unsigned char *sub_ptr  = base_ptr + (g_pan_y * rowbytes) + (g_pan_x * 4);
	sub_ximg.data           = (char*)sub_ptr;

	XPutImage(dpy, win, gc, &sub_ximg, 0, 0, dx, dy, copy_w, copy_h);

	/* Draw command bar if in command mode. */
	if (g_command_mode) {
		int bar_y = win_h - CMD_BAR_HEIGHT;
		XSetForeground(dpy, gc, g_cmdbar_bg_pixel);
		XFillRectangle(dpy, win, gc, 0, bar_y, win_w, CMD_BAR_HEIGHT);

		XSetForeground(dpy, gc, g_text_pixel);
		if (g_cmdFont) {
			XSetFont(dpy, gc, g_cmdFont->fid);
		}
		XDrawString(dpy, win, gc, 5, bar_y + CMD_BAR_HEIGHT - 3,
		            g_command_input, strlen(g_command_input));
	}
}

/*
 * ==================================================
 * Minimal Command-Line Parser & Executor
 * ==================================================
 * When user presses Enter in command mode, we parse:
 *   :<command> <args...>
 * And call the relevant function from commands.h
 */

static void execute_command_line(void)
{
	/* g_command_input e.g. ":convert ~/1.jpg" */
	if (g_command_input[0] != ':') return;

	/* skip the ':' and parse out the first token (the command) */
	char line[1024];
	strncpy(line, g_command_input + 1, sizeof(line));
	line[sizeof(line) - 1] = '\0';

	char *cmd = strtok(line, " \t");
	if (!cmd) {
		return;
	}

	/* The rest is arguments */
	char *args = strtok(NULL, "");
	if (!args) {
		args = (char*)""; /* no arguments typed */
	}

	if (!strcmp(cmd, "convert")) {
		/* usage: :convert <destination> 
		 * calls cmd_convert(g_filename, <destination>)
		 */
		if (*args) {
			cmd_convert(g_filename, args);
		} else {
			fprintf(stderr, "[msxiv] :convert requires a destination\n");
		}

	} else if (!strcmp(cmd, "save")) {
		/* usage: :save 
		 * calls cmd_save(g_filename)
		 */
		cmd_save(g_filename);

	} else if (!strcmp(cmd, "save_as")) {
		/* usage: :save_as <dest> */
		if (*args) {
			cmd_save_as(g_filename, args);
		} else {
			fprintf(stderr, "[msxiv] :save_as requires a destination\n");
		}

	} else if (!strcmp(cmd, "delete")) {
		/* usage: :delete 
		 * calls cmd_delete(g_filename)
		 */
		cmd_delete(g_filename);

	} else if (!strcmp(cmd, "bookmark")) {
		/* usage: :bookmark <label> 
		 * calls cmd_bookmark(g_filename, label, g_config)
		 */
		if (*args) {
			cmd_bookmark(g_filename, args, g_config);
		} else {
			fprintf(stderr, "[msxiv] :bookmark requires a label\n");
		}

	} else {
		fprintf(stderr, "[msxiv] Unknown command: %s\n", cmd);
	}
}

/*
 * =========================
 * PUBLIC API
 * =========================
 */

int viewer_init(Display **dpy, Window *win, ViewerData *vdata, MsxivConfig *config)
{
	MagickWandGenesis();

	g_config = config;
	g_wand   = NULL;
	g_gallery_mode = 0;
	g_thumbs = NULL;
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

	XEvent e;
	while (1) {
		XNextEvent(*dpy, &e);
		if (e.type == MapNotify) break;
	}

	g_cmdFont = XLoadQueryFont(*dpy, CMD_BAR_FONT);
	if (!g_cmdFont) {
		g_cmdFont = XLoadQueryFont(*dpy, "fixed");
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
				/* GALLERY MODE NAVIGATION */
				switch (ks) {
				case XK_q:
					return;
				case XK_Escape:
					g_gallery_mode = 0;
					render_image(dpy, win);
					break;
				case XK_Return:
				case XK_KP_Enter:
					if (g_gallery_select >= 0 && g_gallery_select < vdata->fileCount) {
						vdata->currentIndex = g_gallery_select;
						g_gallery_mode = 0;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					  render_image(dpy, win);
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

					/* Parse & execute the typed command. */
					execute_command_line();

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
				} else if (ks == XK_Tab) {
					try_tab_completion();
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
				/* NORMAL MODE */
				is_ctrl_pressed = ((ev.xkey.state & ControlMask) != 0);

				if (len == 1 && buf[0] == ':') {
					g_command_mode = 1;
					g_command_len  = 1;
					g_command_input[0] = ':';
					g_command_input[1] = '\0';
					render_image(dpy, win);
					break;
				}

				switch (ks) {
				case XK_q:
					return;
				case XK_space:
					if (vdata->currentIndex < vdata->fileCount - 1) {
						vdata->currentIndex++;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					  render_image(dpy, win);
					}
					break;
				case XK_BackSpace:
					if (vdata->currentIndex > 0) {
						vdata->currentIndex--;
						load_image(dpy, win, vdata->files[vdata->currentIndex]);
					  render_image(dpy, win);
					}
					break;
				case XK_Return:
				case XK_KP_Enter:
					if (vdata->fileCount > 1) {
						g_gallery_mode = 1;
						g_gallery_select = vdata->currentIndex;
						render_gallery(dpy, win, vdata);
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
		} break;

		case ButtonPress:
			if (!g_gallery_mode) {
				if (ev.xbutton.button == 4) {
					if (is_ctrl_pressed && g_wand) {
						g_fit_mode = 0;
						g_zoom += ZOOM_STEP;
						if (g_zoom > MAX_ZOOM) g_zoom = MAX_ZOOM;
						generate_scaled_ximg(dpy);
						render_image(dpy, win);
					}
				} else if (ev.xbutton.button == 5) {
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
			/* Could implement drag-based panning if desired. */
			break;
		}
	}
}

void viewer_cleanup(Display *dpy)
{
	free_scaled_ximg();
	/*
	 * If we had vdata->fileCount, we could do:
	 *   free_gallery_thumbnails(vdata->fileCount);
	 */
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
