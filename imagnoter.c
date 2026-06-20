/*
 * Imagnoter - lightweight native GTK4 YOLO bounding-box annotator.
 *
 * Single-file C, GTK4 + Cairo + GdkPixbuf + GLib. No OpenCV / Qt / libadwaita.
 *
 * Build:
 *   gcc imagnoter.c $(pkg-config --cflags --libs gtk4) -O2 -lm -o imagnoter
 *
 * Labels are written as YOLO .txt next to each image (same basename, same
 * folder): "class_id cx cy w h" normalized to [0,1] with 6 decimals.
 * Class names live in classes.txt in the image folder (line index = class id).
 */

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#define UNDO_MAX     50
#define ZOOM_MAX     10.0
#define CACHE_MARGIN 0.30    /* extra fraction of the visible region kept cached on each side */
#define CACHE_MAX_PX 8000    /* clamp on cache surface dimension (defensive) */
#define STATUS_H     26.0
#define TAG_FONT     12.0
#define HANDLE_SZ    4.0

/* resize edge bitmask */
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8

/* interaction modes for the current drag */
enum { MODE_NONE, MODE_DRAW, MODE_MOVE, MODE_RESIZE, MODE_PAN };

typedef struct {
    double cx, cy, w, h;   /* normalized [0,1], center + size */
    int class_id;
} Box;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *area;            /* GtkDrawingArea (canvas) */
    GtkWidget *revealer;        /* left panel revealer */
    GtkWidget *panel;           /* left panel box (popover parent) */
    GtkWidget *list;            /* GtkListBox of classes */
    GtkWidget *folder_label;    /* header bar folder name */
    GtkWidget *panel_btn;
    GtkWidget *add_pop;         /* add-class popover */
    GtkWidget *add_entry;
    GtkWidget *rename_pop;
    GtkWidget *rename_entry;
    int rename_target;          /* class id being renamed, -1 none */

    /* dataset */
    char *folder;
    GPtrArray *images;          /* char* basenames, readdir order */
    int cur;                    /* current image index, -1 none */
    gboolean folder_ro;         /* folder not writable */

    /* current image */
    GdkPixbuf *pixbuf;
    int img_w, img_h;

    /* cached pre-scaled view (region of pixbuf scaled to current zoom).
       Rebuilt only on zoom change / image change / pan out of margin, so the
       per-frame path is a 1:1 blit instead of a full resample. */
    GdkPixbuf *cache;
    GdkPixbuf *cache_src;       /* pixbuf the cache was built from (identity guard) */
    double cache_zoom;
    double cache_ix0, cache_iy0, cache_iw, cache_ih;  /* cached region, image px */

    char last_title[256];       /* avoid redundant set_title every frame */

    /* boxes of current image (normalized), live working copy */
    GArray *boxes;
    int active_class;           /* -1 none */
    int selected;               /* selected box index, -1 none */
    gboolean dirty;             /* unsaved changes on current image */
    gboolean show_tags;

    /* view transform: screen = image_px * zoom + offset */
    double zoom, off_x, off_y;
    double fit_zoom;

    /* classes */
    GPtrArray *classes;         /* char* names (never deleted) */

    /* settings */
    double edge_tol;            /* screen px */
    double min_edge;            /* image px, per edge */
    gboolean save_empty;        /* write empty .txt on pass-through */

    /* undo/redo: history of box-list snapshots; history[idx] == live */
    GPtrArray *history;         /* GArray* snapshots */
    int hist_idx;

    /* touch sticky modifier modes (overlay buttons) */
    gboolean touch_edit;
    gboolean touch_del;

    /* drag state */
    gboolean btn_down;
    int press_button;
    GdkModifierType press_mods;
    int mode;
    double start_sx, start_sy;  /* screen px at press */
    double draw_x0, draw_y0, draw_x1, draw_y1;  /* image px provisional */
    int edit_box;
    int resize_edges;
    Box edit_orig;              /* original box at drag-begin (image px in fields) */
    double grab_dx, grab_dy;    /* image px from box origin to grab point */
    double pan_ox, pan_oy;      /* offset at pan begin */
    double pinch_zoom0;         /* zoom at pinch begin */

    char *config_path;
} App;

static App app;

/* ---- forward declarations ---- */
static void redraw(void);
static void load_image_at(int idx);
static void save_current(void);
static void build_class_list(void);
static void push_undo(void);
static void update_title(void);
static void report_error(const char *fmt, ...);
static void on_panel_toggle(GtkButton *b, gpointer u);

/* ------------------------------------------------------------------ */
/* colors                                                              */
/* ------------------------------------------------------------------ */

static void hsv2rgb(double h, double s, double v, double *r, double *g, double *b)
{
    double c=v*s;
    double hp=h/60.0;
    double x=c*(1.0-fabs(fmod(hp,2.0)-1.0));
    double r1=0, g1=0, b1=0;
    if(hp<1){ r1=c; g1=x; }
    else if(hp<2){ r1=x; g1=c; }
    else if(hp<3){ g1=c; b1=x; }
    else if(hp<4){ g1=x; b1=c; }
    else if(hp<5){ r1=x; b1=c; }
    else { r1=c; b1=x; }
    double m=v-c;
    *r=r1+m; *g=g1+m; *b=b1+m;
}

/* per-class color: hand-picked palette first, golden-angle hue beyond */
static void class_color(int id, double *r, double *g, double *b)
{
    static const double pal[][3]={
        {0.90,0.20,0.20}, {0.20,0.70,0.30}, {0.25,0.45,0.95},
        {0.95,0.60,0.10}, {0.60,0.30,0.80}, {0.10,0.75,0.80},
        {0.90,0.30,0.70}, {0.80,0.75,0.15}, {0.15,0.55,0.55},
        {0.95,0.45,0.55}
    };
    int n=(int)(sizeof(pal)/sizeof(pal[0]));
    if(id>=0 && id<n){ *r=pal[id][0]; *g=pal[id][1]; *b=pal[id][2]; return; }
    double hue=fmod((double)id*137.508, 360.0);
    hsv2rgb(hue, 0.65, 0.95, r, g, b);
}

/* ------------------------------------------------------------------ */
/* view transform helpers                                              */
/* ------------------------------------------------------------------ */

static double sx2ix(double sx){ return (sx-app.off_x)/app.zoom; }
static double sy2iy(double sy){ return (sy-app.off_y)/app.zoom; }
static double ix2sx(double ix){ return ix*app.zoom+app.off_x; }
static double iy2sy(double iy){ return iy*app.zoom+app.off_y; }

static double clampd(double v, double lo, double hi)
{
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}

static void compute_fit(void)
{
    if(!app.pixbuf) return;
    int aw=gtk_widget_get_width(app.area);
    int ah=gtk_widget_get_height(app.area);
    if(aw<=0) aw=800;
    if(ah<=0) ah=600;
    double zx=(double)aw/app.img_w;
    double zy=(double)ah/app.img_h;
    app.fit_zoom=zx<zy?zx:zy;
    app.zoom=app.fit_zoom;
    app.off_x=(aw-app.img_w*app.zoom)/2.0;
    app.off_y=(ah-app.img_h*app.zoom)/2.0;
}

static void zoom_at(double factor, double sx, double sy)
{
    double ix=sx2ix(sx), iy=sy2iy(sy);
    double nz=clampd(app.zoom*factor, app.fit_zoom, ZOOM_MAX*app.fit_zoom);
    app.zoom=nz;
    app.off_x=sx-ix*app.zoom;
    app.off_y=sy-iy*app.zoom;
    redraw();
}

/* ------------------------------------------------------------------ */
/* box geometry (in screen space, for hit-testing)                     */
/* ------------------------------------------------------------------ */

static void box_screen_rect(const Box *b, double *x0, double *y0, double *x1, double *y1)
{
    double bx0=(b->cx-b->w/2.0)*app.img_w;
    double by0=(b->cy-b->h/2.0)*app.img_h;
    double bx1=(b->cx+b->w/2.0)*app.img_w;
    double by1=(b->cy+b->h/2.0)*app.img_h;
    *x0=ix2sx(bx0); *y0=iy2sy(by0);
    *x1=ix2sx(bx1); *y1=iy2sy(by1);
}

/* smallest box whose (tol-expanded) screen rect contains the point */
static int hittest(double sx, double sy)
{
    int best=-1;
    double best_area=1e30;
    double tol=app.edge_tol;
    for(guint i=0;i<app.boxes->len;i++){
        Box *b=&g_array_index(app.boxes, Box, i);
        double x0,y0,x1,y1;
        box_screen_rect(b,&x0,&y0,&x1,&y1);
        if(sx>=x0-tol && sx<=x1+tol && sy>=y0-tol && sy<=y1+tol){
            double a=(x1-x0)*(y1-y0);
            if(a<best_area){ best_area=a; best=(int)i; }
        }
    }
    return best;
}

/* which edges of box i are within tol of the point (screen space) */
static int edge_mask(int i, double sx, double sy)
{
    Box *b=&g_array_index(app.boxes, Box, i);
    double x0,y0,x1,y1;
    box_screen_rect(b,&x0,&y0,&x1,&y1);
    double tol=app.edge_tol;
    int m=0;
    if(fabs(sx-x0)<=tol && sy>=y0-tol && sy<=y1+tol) m|=EDGE_L;
    if(fabs(sx-x1)<=tol && sy>=y0-tol && sy<=y1+tol) m|=EDGE_R;
    if(fabs(sy-y0)<=tol && sx>=x0-tol && sx<=x1+tol) m|=EDGE_T;
    if(fabs(sy-y1)<=tol && sx>=x0-tol && sx<=x1+tol) m|=EDGE_B;
    /* tiny box: opposite edges both grabbed -> treat that axis as move */
    if((m&EDGE_L) && (m&EDGE_R)) m&=~(EDGE_L|EDGE_R);
    if((m&EDGE_T) && (m&EDGE_B)) m&=~(EDGE_T|EDGE_B);
    return m;
}

/* ------------------------------------------------------------------ */
/* undo / redo                                                         */
/* ------------------------------------------------------------------ */

static GArray *snapshot_copy(GArray *src)
{
    GArray *d=g_array_new(FALSE, FALSE, sizeof(Box));
    if(src->len) g_array_append_vals(d, src->data, src->len);
    return d;
}

static void clear_history(void)
{
    for(guint i=0;i<app.history->len;i++)
        g_array_free(g_ptr_array_index(app.history,i), TRUE);
    g_ptr_array_set_size(app.history, 0);
    app.hist_idx=-1;
}

static void push_undo(void)
{
    /* drop redo tail */
    while((int)app.history->len > app.hist_idx+1){
        GArray *g=g_ptr_array_index(app.history, app.history->len-1);
        g_array_free(g, TRUE);
        g_ptr_array_remove_index(app.history, app.history->len-1);
    }
    g_ptr_array_add(app.history, snapshot_copy(app.boxes));
    app.hist_idx=app.history->len-1;
    /* cap */
    while(app.history->len > UNDO_MAX){
        GArray *g=g_ptr_array_index(app.history, 0);
        g_array_free(g, TRUE);
        g_ptr_array_remove_index(app.history, 0);
        app.hist_idx--;
    }
}

static void restore_from(int idx)
{
    GArray *snap=g_ptr_array_index(app.history, idx);
    g_array_set_size(app.boxes, 0);
    if(snap->len) g_array_append_vals(app.boxes, snap->data, snap->len);
    app.selected=-1;
}

static void do_undo(void)
{
    if(app.hist_idx>0){
        app.hist_idx--;
        restore_from(app.hist_idx);
        app.dirty=TRUE;
        redraw();
    }
}

static void do_redo(void)
{
    if(app.hist_idx<(int)app.history->len-1){
        app.hist_idx++;
        restore_from(app.hist_idx);
        app.dirty=TRUE;
        redraw();
    }
}

/* ------------------------------------------------------------------ */
/* classes.txt load / save (atomic)                                    */
/* ------------------------------------------------------------------ */

static void classes_save(void)
{
    if(!app.folder || app.folder_ro) return;
    char *tmp=g_build_filename(app.folder, ".classes.txt.tmp", NULL);
    char *dst=g_build_filename(app.folder, "classes.txt", NULL);
    FILE *f=g_fopen(tmp, "w");
    gboolean ok=(f!=NULL);
    if(ok){
        for(guint i=0;i<app.classes->len;i++){
            const char *nm=g_ptr_array_index(app.classes, i);
            if(fprintf(f, "%s\n", nm)<0){ ok=FALSE; break; }
        }
        if(fclose(f)!=0) ok=FALSE;
    }
    if(ok){
        if(g_rename(tmp, dst)!=0) ok=FALSE;
    }
    if(!ok){
        g_remove(tmp);
        report_error("Could not write classes.txt: %s", g_strerror(errno));
    }
    g_free(tmp);
    g_free(dst);
}

static void classes_load(void)
{
    g_ptr_array_set_size(app.classes, 0);
    if(!app.folder) return;
    char *path=g_build_filename(app.folder, "classes.txt", NULL);
    char *data=NULL;
    if(g_file_get_contents(path, &data, NULL, NULL)){
        char **lines=g_strsplit(data, "\n", -1);
        for(int i=0;lines[i];i++){
            /* keep every line as-is; trailing empty split element is dropped */
            if(lines[i+1]==NULL && lines[i][0]=='\0') break;
            g_ptr_array_add(app.classes, g_strdup(lines[i]));
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* label load / save                                                   */
/* ------------------------------------------------------------------ */

static char *label_path_for(const char *image_basename)
{
    char *dot=strrchr(image_basename, '.');
    char *stem=dot?g_strndup(image_basename,(gsize)(dot-image_basename)):g_strdup(image_basename);
    char *txt=g_strconcat(stem, ".txt", NULL);
    char *full=g_build_filename(app.folder, txt, NULL);
    g_free(stem); g_free(txt);
    return full;
}

static void labels_load(const char *image_basename)
{
    g_array_set_size(app.boxes, 0);
    char *path=label_path_for(image_basename);
    char *data=NULL;
    if(g_file_get_contents(path, &data, NULL, NULL)){
        char **lines=g_strsplit(data, "\n", -1);
        for(int i=0;lines[i];i++){
            if(lines[i][0]=='\0') continue;
            Box b; int id;
            if(sscanf(lines[i], "%d %lf %lf %lf %lf",
                      &id, &b.cx, &b.cy, &b.w, &b.h)==5){
                b.class_id=id;
                /* clamp defensively */
                b.cx=clampd(b.cx,0,1); b.cy=clampd(b.cy,0,1);
                b.w =clampd(b.w,0,1);  b.h =clampd(b.h,0,1);
                g_array_append_val(app.boxes, b);
            }
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
}

static void save_current(void)
{
    if(app.cur<0 || !app.folder || app.folder_ro) return;
    const char *bn=g_ptr_array_index(app.images, app.cur);
    char *path=label_path_for(bn);

    if(app.boxes->len==0 && !app.save_empty){
        /* no boxes, negatives disabled: drop any stale label */
        if(g_file_test(path, G_FILE_TEST_EXISTS)) g_remove(path);
        app.dirty=FALSE;
        g_free(path);
        return;
    }

    FILE *f=g_fopen(path, "w");
    gboolean ok=(f!=NULL);
    if(ok){
        for(guint i=0;i<app.boxes->len;i++){
            Box *b=&g_array_index(app.boxes, Box, i);
            if(fprintf(f, "%d %.6f %.6f %.6f %.6f\n",
                       b->class_id, b->cx, b->cy, b->w, b->h)<0){ ok=FALSE; break; }
        }
        if(fclose(f)!=0) ok=FALSE;
    }
    if(!ok)
        report_error("Could not write %s: %s", bn, g_strerror(errno));
    else
        app.dirty=FALSE;
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void redraw(void){ gtk_widget_queue_draw(app.area); update_title(); }

/* Build/refresh the scaled-view cache if needed. The cache holds the visible
   image region (plus a margin) pre-scaled to the current zoom, so on_draw only
   blits it 1:1. The expensive scale runs here, and only when the source, the
   zoom, or the visible region (beyond margin) actually changes. */
static void ensure_cache(void)
{
    if(!app.pixbuf) return;
    int aw=gtk_widget_get_width(app.area);
    int ah=gtk_widget_get_height(app.area);
    if(aw<=0 || ah<=0) return;

    /* visible image region in image px, clamped to the image */
    double vx0=clampd(sx2ix(0),  0, app.img_w);
    double vy0=clampd(sy2iy(0),  0, app.img_h);
    double vx1=clampd(sx2ix(aw), 0, app.img_w);
    double vy1=clampd(sy2iy(ah), 0, app.img_h);
    if(vx1-vx0<1.0 || vy1-vy0<1.0) return;   /* nothing meaningful visible */

    /* reuse cache if same source, same zoom, and visible region still inside it */
    if(app.cache && app.cache_src==app.pixbuf &&
       fabs(app.cache_zoom-app.zoom)<1e-9 &&
       vx0>=app.cache_ix0 && vy0>=app.cache_iy0 &&
       vx1<=app.cache_ix0+app.cache_iw &&
       vy1<=app.cache_iy0+app.cache_ih)
        return;

    /* desired region = visible expanded by margin, clamped to image bounds */
    double mw=(vx1-vx0)*CACHE_MARGIN;
    double mh=(vy1-vy0)*CACHE_MARGIN;
    double dix0=floor(clampd(vx0-mw, 0, app.img_w));
    double diy0=floor(clampd(vy0-mh, 0, app.img_h));
    double dix1=ceil (clampd(vx1+mw, 0, app.img_w));
    double diy1=ceil (clampd(vy1+mh, 0, app.img_h));
    int riw=(int)(dix1-dix0);
    int rih=(int)(diy1-diy0);
    if(riw<1 || rih<1) return;

    int dw=(int)lround((dix1-dix0)*app.zoom);
    int dh=(int)lround((diy1-diy0)*app.zoom);
    if(dw<1) dw=1;
    if(dh<1) dh=1;
    if(dw>CACHE_MAX_PX) dw=CACHE_MAX_PX;
    if(dh>CACHE_MAX_PX) dh=CACHE_MAX_PX;

    GdkPixbuf *sub=gdk_pixbuf_new_subpixbuf(app.pixbuf,(int)dix0,(int)diy0,riw,rih);
    if(!sub) return;
    /* quality filter here is fine: this runs rarely, not per frame.
       BILINEAR keeps zoom-step latency low; switch downscale to GDK_INTERP_HYPER
       for nicer fit quality at the cost of a one-time rebuild hitch. */
    GdkInterpType interp=GDK_INTERP_BILINEAR;
    GdkPixbuf *scaled=gdk_pixbuf_scale_simple(sub, dw, dh, interp);
    g_object_unref(sub);
    if(!scaled) return;

    if(app.cache) g_object_unref(app.cache);
    app.cache=scaled;
    app.cache_src=app.pixbuf;
    app.cache_zoom=app.zoom;
    app.cache_ix0=dix0; app.cache_iy0=diy0;
    app.cache_iw=dix1-dix0; app.cache_ih=diy1-diy0;
}

static void cache_clear(void)
{
    if(app.cache){ g_object_unref(app.cache); app.cache=NULL; }
    app.cache_src=NULL;
}

static void draw_text_tag(cairo_t *cr, double x, double y,
                          const char *txt, double r, double g, double b)
{
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, TAG_FONT);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    double pad=3.0;
    double tw=ext.width+2*pad;
    double th=TAG_FONT+2*pad;
    /* background */
    cairo_set_source_rgba(cr, r, g, b, 0.85);
    cairo_rectangle(cr, x, y-th, tw, th);
    cairo_fill(cr);
    /* text in black or white per luminance */
    double lum=0.299*r+0.587*g+0.114*b;
    if(lum>0.6) cairo_set_source_rgb(cr, 0, 0, 0);
    else        cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, x+pad, y-pad);
    cairo_show_text(cr, txt);
}

static void on_draw(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer u)
{
    (void)da; (void)u;
    /* background */
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    cairo_paint(cr);

    if(!app.pixbuf){
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 16);
        const char *msg="Open a folder to start  (Open folder button, top-left)";
        cairo_text_extents_t e;
        cairo_text_extents(cr, msg, &e);
        cairo_move_to(cr, (w-e.width)/2.0, h/2.0);
        cairo_show_text(cr, msg);
        return;
    }

    /* image: blit the pre-scaled cache 1:1 (no per-frame resample) */
    ensure_cache();
    if(app.cache){
        double bx=ix2sx(app.cache_ix0);
        double by=iy2sy(app.cache_iy0);
        cairo_save(cr);
        cairo_translate(cr, bx, by);
        gdk_cairo_set_source_pixbuf(cr, app.cache, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    /* boxes */
    for(guint i=0;i<app.boxes->len;i++){
        Box *b=&g_array_index(app.boxes, Box, i);
        double r,g,bl;
        class_color(b->class_id,&r,&g,&bl);
        double x0,y0,x1,y1;
        box_screen_rect(b,&x0,&y0,&x1,&y1);
        gboolean sel=((int)i==app.selected);
        cairo_set_line_width(cr, sel?3.0:2.0);
        cairo_set_source_rgb(cr, r, g, bl);
        cairo_rectangle(cr, x0, y0, x1-x0, y1-y0);
        cairo_stroke(cr);
        if(sel){
            /* corner handles */
            cairo_set_source_rgb(cr, 1, 1, 1);
            double hs=HANDLE_SZ;
            double cxs[4]={x0,x1,x0,x1}, cys[4]={y0,y0,y1,y1};
            for(int k=0;k<4;k++){
                cairo_rectangle(cr, cxs[k]-hs, cys[k]-hs, 2*hs, 2*hs);
                cairo_fill(cr);
            }
        }
        if(app.show_tags){
            const char *nm="?";
            if(b->class_id>=0 && b->class_id<(int)app.classes->len)
                nm=g_ptr_array_index(app.classes, b->class_id);
            draw_text_tag(cr, x0, y0, nm, r, g, bl);
        }
    }

    /* provisional box while drawing */
    if(app.btn_down && app.mode==MODE_DRAW){
        double r,g,bl;
        class_color(app.active_class,&r,&g,&bl);
        double x0=ix2sx(fmin(app.draw_x0,app.draw_x1));
        double y0=iy2sy(fmin(app.draw_y0,app.draw_y1));
        double x1=ix2sx(fmax(app.draw_x0,app.draw_x1));
        double y1=iy2sy(fmax(app.draw_y0,app.draw_y1));
        double dash[]={6.0,4.0};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, 2.0);
        cairo_set_source_rgb(cr, r, g, bl);
        cairo_rectangle(cr, x0, y0, x1-x0, y1-y0);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }

    /* status strip */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_rectangle(cr, 0, h-STATUS_H, w, STATUS_H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    char buf[256];
    const char *acn="(none)";
    if(app.active_class>=0 && app.active_class<(int)app.classes->len)
        acn=g_ptr_array_index(app.classes, app.active_class);
    snprintf(buf, sizeof(buf), "%d/%d   %.0f%%   class: %s   %s%s",
             app.cur+1, app.images?app.images->len:0,
             app.zoom/app.fit_zoom*100.0, acn,
             app.dirty?"\xe2\x97\x8f unsaved":"",
             app.folder_ro?"   [read-only]":"");
    cairo_move_to(cr, 8, h-STATUS_H/2.0+5);
    cairo_show_text(cr, buf);
}

/* ------------------------------------------------------------------ */
/* box editing primitives                                              */
/* ------------------------------------------------------------------ */

static void box_to_imgpx(const Box *b, double *x0, double *y0, double *x1, double *y1)
{
    *x0=(b->cx-b->w/2.0)*app.img_w;
    *y0=(b->cy-b->h/2.0)*app.img_h;
    *x1=(b->cx+b->w/2.0)*app.img_w;
    *y1=(b->cy+b->h/2.0)*app.img_h;
}

static void imgpx_to_box(Box *b, double x0, double y0, double x1, double y1)
{
    if(x1<x0){ double t=x0; x0=x1; x1=t; }
    if(y1<y0){ double t=y0; y0=y1; y1=t; }
    x0=clampd(x0,0,app.img_w); x1=clampd(x1,0,app.img_w);
    y0=clampd(y0,0,app.img_h); y1=clampd(y1,0,app.img_h);
    b->cx=((x0+x1)/2.0)/app.img_w;
    b->cy=((y0+y1)/2.0)/app.img_h;
    b->w =(x1-x0)/app.img_w;
    b->h =(y1-y0)/app.img_h;
}

/* ------------------------------------------------------------------ */
/* pointer interaction                                                 */
/* ------------------------------------------------------------------ */

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void)n; (void)u;
    if(!app.pixbuf) return;
    int button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    GdkModifierType mods=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));

    app.btn_down=TRUE;
    app.press_button=button;
    app.press_mods=mods;
    app.start_sx=x; app.start_sy=y;
    app.mode=MODE_NONE;

    gboolean want_ctrl=(mods&GDK_CONTROL_MASK) || app.touch_edit;
    gboolean want_shift=(mods&GDK_SHIFT_MASK) || app.touch_del;

    if(button==GDK_BUTTON_SECONDARY){
        app.mode=MODE_PAN;
        app.pan_ox=app.off_x; app.pan_oy=app.off_y;
        return;
    }

    if(want_shift){
        int hit=hittest(x,y);
        if(hit>=0){
            g_array_remove_index(app.boxes, hit);
            if(app.selected==hit) app.selected=-1;
            else if(app.selected>hit) app.selected--;
            push_undo();
            app.dirty=TRUE;
            redraw();
        }
        app.mode=MODE_NONE;
        return;
    }

    if(want_ctrl){
        int hit=hittest(x,y);
        if(hit>=0){
            app.selected=hit;
            int em=edge_mask(hit,x,y);
            app.edit_box=hit;
            app.edit_orig=g_array_index(app.boxes, Box, hit);
            if(em){
                app.mode=MODE_RESIZE;
                app.resize_edges=em;
            }else{
                app.mode=MODE_MOVE;
                double x0,y0,x1,y1;
                box_to_imgpx(&app.edit_orig,&x0,&y0,&x1,&y1);
                app.grab_dx=sx2ix(x)-x0;
                app.grab_dy=sy2iy(y)-y0;
            }
        }else{
            app.selected=-1;
        }
        redraw();
        return;
    }

    /* default: draw a new box (requires a valid active class) */
    if(app.active_class>=0 && app.active_class<(int)app.classes->len){
        app.mode=MODE_DRAW;
        double ix=clampd(sx2ix(x),0,app.img_w);
        double iy=clampd(sy2iy(y),0,app.img_h);
        app.draw_x0=app.draw_x1=ix;
        app.draw_y0=app.draw_y1=iy;
    }
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer u)
{
    (void)c; (void)u;
    if(!app.btn_down || !app.pixbuf) return;

    if(app.mode==MODE_PAN){
        app.off_x=app.pan_ox+(x-app.start_sx);
        app.off_y=app.pan_oy+(y-app.start_sy);
        redraw();
        return;
    }
    if(app.mode==MODE_DRAW){
        app.draw_x1=clampd(sx2ix(x),0,app.img_w);
        app.draw_y1=clampd(sy2iy(y),0,app.img_h);
        redraw();
        return;
    }
    if(app.mode==MODE_MOVE){
        double ox0,oy0,ox1,oy1;
        box_to_imgpx(&app.edit_orig,&ox0,&oy0,&ox1,&oy1);
        double bw=ox1-ox0, bh=oy1-oy0;
        double nx0=sx2ix(x)-app.grab_dx;
        double ny0=sy2iy(y)-app.grab_dy;
        nx0=clampd(nx0, 0, app.img_w-bw);
        ny0=clampd(ny0, 0, app.img_h-bh);
        Box *b=&g_array_index(app.boxes, Box, app.edit_box);
        imgpx_to_box(b, nx0, ny0, nx0+bw, ny0+bh);
        app.dirty=TRUE;
        redraw();
        return;
    }
    if(app.mode==MODE_RESIZE){
        double x0,y0,x1,y1;
        box_to_imgpx(&app.edit_orig,&x0,&y0,&x1,&y1);
        double ix=clampd(sx2ix(x),0,app.img_w);
        double iy=clampd(sy2iy(y),0,app.img_h);
        if(app.resize_edges&EDGE_L) x0=ix;
        if(app.resize_edges&EDGE_R) x1=ix;
        if(app.resize_edges&EDGE_T) y0=iy;
        if(app.resize_edges&EDGE_B) y1=iy;
        /* enforce min edge (clamp, do not collapse) */
        if(fabs(x1-x0)<app.min_edge){
            if(app.resize_edges&EDGE_L) x0=x1-app.min_edge;
            else if(app.resize_edges&EDGE_R) x1=x0+app.min_edge;
        }
        if(fabs(y1-y0)<app.min_edge){
            if(app.resize_edges&EDGE_T) y0=y1-app.min_edge;
            else if(app.resize_edges&EDGE_B) y1=y0+app.min_edge;
        }
        Box *b=&g_array_index(app.boxes, Box, app.edit_box);
        imgpx_to_box(b, x0, y0, x1, y1);
        app.dirty=TRUE;
        redraw();
        return;
    }
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void)g; (void)n; (void)x; (void)y; (void)u;
    if(!app.btn_down) return;
    app.btn_down=FALSE;

    if(app.mode==MODE_DRAW){
        double x0=fmin(app.draw_x0,app.draw_x1);
        double y0=fmin(app.draw_y0,app.draw_y1);
        double x1=fmax(app.draw_x0,app.draw_x1);
        double y1=fmax(app.draw_y0,app.draw_y1);
        if((x1-x0)>=app.min_edge && (y1-y0)>=app.min_edge){
            Box b;
            imgpx_to_box(&b, x0, y0, x1, y1);
            b.class_id=app.active_class;
            g_array_append_val(app.boxes, b);
            app.selected=app.boxes->len-1;
            push_undo();
            app.dirty=TRUE;
        }
        redraw();
    }else if(app.mode==MODE_MOVE || app.mode==MODE_RESIZE){
        push_undo();
        redraw();
    }
    app.mode=MODE_NONE;
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer u)
{
    (void)dx; (void)u;
    if(!app.pixbuf) return FALSE;
    GtkEventController *ec=GTK_EVENT_CONTROLLER(c);
    double sx=0, sy=0;
    /* pointer position is tracked via motion; fall back to widget center */
    /* use last known via separate motion tracking */
    sx=app.start_sx; sy=app.start_sy;
    (void)ec;
    if(sx==0 && sy==0){
        sx=gtk_widget_get_width(app.area)/2.0;
        sy=gtk_widget_get_height(app.area)/2.0;
    }
    double factor=(dy<0)?1.1:(1.0/1.1);
    zoom_at(factor, sx, sy);
    return TRUE;
}

/* keep a live pointer position for cursor-anchored wheel zoom */
static void on_hover(GtkEventControllerMotion *c, double x, double y, gpointer u)
{
    (void)c; (void)u;
    if(!app.btn_down){ app.start_sx=x; app.start_sy=y; }
}

/* pinch zoom (touch) */
static void on_pinch_begin(GtkGesture *g, GdkEventSequence *s, gpointer u)
{
    (void)s; (void)u; (void)g;
    app.pinch_zoom0=app.zoom;
}
static void on_pinch_scale(GtkGestureZoom *g, double scale, gpointer u)
{
    (void)u;
    if(!app.pixbuf) return;
    double cx, cy;
    if(!gtk_gesture_get_bounding_box_center(GTK_GESTURE(g), &cx, &cy)){
        cx=gtk_widget_get_width(app.area)/2.0;
        cy=gtk_widget_get_height(app.area)/2.0;
    }
    double ix=sx2ix(cx), iy=sy2iy(cy);
    app.zoom=clampd(app.pinch_zoom0*scale, app.fit_zoom, ZOOM_MAX*app.fit_zoom);
    app.off_x=cx-ix*app.zoom;
    app.off_y=cy-iy*app.zoom;
    redraw();
}

/* ------------------------------------------------------------------ */
/* navigation                                                          */
/* ------------------------------------------------------------------ */

static void go_to(int idx)
{
    if(!app.images || app.images->len==0) return;
    if(idx<0) idx=0;
    if(idx>=(int)app.images->len) idx=app.images->len-1;
    if(idx==app.cur) return;
    save_current();
    load_image_at(idx);
}

static void load_image_at(int idx)
{
    if(app.pixbuf){ g_object_unref(app.pixbuf); app.pixbuf=NULL; }
    cache_clear();
    app.cur=idx;
    app.selected=-1;
    app.dirty=FALSE;
    if(idx<0 || idx>=(int)app.images->len){ redraw(); return; }
    const char *bn=g_ptr_array_index(app.images, idx);
    char *path=g_build_filename(app.folder, bn, NULL);
    GError *err=NULL;
    app.pixbuf=gdk_pixbuf_new_from_file(path, &err);
    g_free(path);
    if(!app.pixbuf){
        report_error("Cannot load %s: %s", bn, err?err->message:"unknown");
        if(err) g_error_free(err);
        /* skip forward to a loadable image if possible */
        g_array_set_size(app.boxes, 0);
        clear_history();
        push_undo();
        redraw();
        return;
    }
    app.img_w=gdk_pixbuf_get_width(app.pixbuf);
    app.img_h=gdk_pixbuf_get_height(app.pixbuf);
    labels_load(bn);
    clear_history();
    push_undo();              /* baseline state */
    compute_fit();
    redraw();
}

/* ------------------------------------------------------------------ */
/* folder scan                                                         */
/* ------------------------------------------------------------------ */

static gboolean is_image_name(const char *n)
{
    const char *dot=strrchr(n, '.');
    if(!dot) return FALSE;
    char *low=g_ascii_strdown(dot+1, -1);
    gboolean ok=(!strcmp(low,"jpg") || !strcmp(low,"jpeg") ||
                 !strcmp(low,"png") || !strcmp(low,"bmp"));
    g_free(low);
    return ok;
}

static void open_folder(const char *path, const char *resume_name)
{
    /* save anything pending in the previous folder */
    save_current();

    g_free(app.folder);
    app.folder=g_strdup(path);
    app.folder_ro=(access(path, W_OK)!=0);

    g_ptr_array_set_size(app.images, 0);
    GDir *d=g_dir_open(path, 0, NULL);
    if(d){
        const char *name;
        while((name=g_dir_read_name(d))){   /* raw readdir order, no sort */
            if(is_image_name(name))
                g_ptr_array_add(app.images, g_strdup(name));
        }
        g_dir_close(d);
    }

    classes_load();
    if(app.classes->len>0 && app.active_class<0) app.active_class=0;
    build_class_list();

    /* resume position by filename */
    int start=0;
    if(resume_name){
        for(guint i=0;i<app.images->len;i++){
            if(!strcmp(g_ptr_array_index(app.images,i), resume_name)){ start=(int)i; break; }
        }
    }

    if(app.folder_label){
        char *base=g_path_get_basename(path);
        gtk_label_set_text(GTK_LABEL(app.folder_label), base);
        g_free(base);
    }

    app.cur=-1;
    if(app.images->len>0) load_image_at(start);
    else { if(app.pixbuf){ g_object_unref(app.pixbuf); app.pixbuf=NULL; } cache_clear(); redraw(); }

    if(app.folder_ro)
        report_error("Folder is read-only - annotations will not be saved.");
}

/* GtkFileDialog async callback */
static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer u)
{
    (void)u;
    GError *err=NULL;
    GFile *file=gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if(file){
        char *path=g_file_get_path(file);
        if(path){ open_folder(path, NULL); g_free(path); }
        g_object_unref(file);
    }
    if(err) g_error_free(err);
}

static void on_open_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    GtkFileDialog *dlg=gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Open image folder");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(app.window), NULL,
                                  on_folder_chosen, NULL);
    g_object_unref(dlg);
}

/* ------------------------------------------------------------------ */
/* class panel                                                         */
/* ------------------------------------------------------------------ */

static void swatch_draw(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer u)
{
    (void)da;
    int id=GPOINTER_TO_INT(u);
    double r,g,b;
    class_color(id,&r,&g,&b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, 1, 1, w-2, h-2);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 1.5, 1.5, w-3, h-3);
    cairo_stroke(cr);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer u)
{
    (void)lb; (void)u;
    if(!row) return;
    int id=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "class-id"));
    app.active_class=id;
    redraw();
}

static void build_class_list(void)
{
    /* remove only actual rows; the rename popover is also a child of the
       list (parented for positioning) and must be left in place */
    GtkListBoxRow *r;
    while((r=gtk_list_box_get_row_at_index(GTK_LIST_BOX(app.list), 0)))
        gtk_list_box_remove(GTK_LIST_BOX(app.list), GTK_WIDGET(r));

    for(guint i=0;i<app.classes->len;i++){
        const char *nm=g_ptr_array_index(app.classes, i);
        GtkWidget *rowbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(rowbox,6);
        gtk_widget_set_margin_end(rowbox,6);
        gtk_widget_set_margin_top(rowbox,4);
        gtk_widget_set_margin_bottom(rowbox,4);

        GtkWidget *sw=gtk_drawing_area_new();
        gtk_widget_set_size_request(sw,18,18);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sw), swatch_draw,
                                       GINT_TO_POINTER((int)i), NULL);
        gtk_box_append(GTK_BOX(rowbox), sw);

        char *lbl=g_strdup_printf("%u: %s", i, nm);
        GtkWidget *l=gtk_label_new(lbl);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(rowbox), l);
        g_free(lbl);

        GtkWidget *row=gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rowbox);
        g_object_set_data(G_OBJECT(row), "class-id", GINT_TO_POINTER((int)i));
        gtk_list_box_append(GTK_LIST_BOX(app.list), row);

        if((int)i==app.active_class)
            gtk_list_box_select_row(GTK_LIST_BOX(app.list), GTK_LIST_BOX_ROW(row));
    }
}

/* add class popover */
static void on_add_commit(GtkEntry *e, gpointer u)
{
    (void)u;
    const char *txt=gtk_editable_get_text(GTK_EDITABLE(e));
    if(txt && txt[0]!='\0'){
        g_ptr_array_add(app.classes, g_strdup(txt));
        app.active_class=app.classes->len-1;
        classes_save();
        build_class_list();
        redraw();
    }
    gtk_editable_set_text(GTK_EDITABLE(e), "");
    gtk_popover_popdown(GTK_POPOVER(app.add_pop));
}

static void on_add_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    gtk_editable_set_text(GTK_EDITABLE(app.add_entry), "");
    gtk_popover_popup(GTK_POPOVER(app.add_pop));
    gtk_widget_grab_focus(app.add_entry);
}

/* rename popover */
static void on_rename_commit(GtkEntry *e, gpointer u)
{
    (void)u;
    const char *txt=gtk_editable_get_text(GTK_EDITABLE(e));
    if(app.rename_target>=0 && app.rename_target<(int)app.classes->len &&
       txt && txt[0]!='\0'){
        g_free(g_ptr_array_index(app.classes, app.rename_target));
        app.classes->pdata[app.rename_target]=g_strdup(txt);
        classes_save();
        build_class_list();
        redraw();
    }
    gtk_popover_popdown(GTK_POPOVER(app.rename_pop));
}

static void open_rename(int id, double x, double y)
{
    if(id<0 || id>=(int)app.classes->len) return;
    app.rename_target=id;
    gtk_editable_set_text(GTK_EDITABLE(app.rename_entry),
                          g_ptr_array_index(app.classes, id));
    /* the gesture reports coords in list space; the popover lives on the
       panel box, so translate (accounts for scroll offset too) */
    graphene_point_t in={ (float)x, (float)y }, out;
    if(!gtk_widget_compute_point(app.list, app.panel, &in, &out)){
        out.x=(float)x; out.y=(float)y;
    }
    GdkRectangle rect={ (int)out.x, (int)out.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(app.rename_pop), &rect);
    gtk_popover_popup(GTK_POPOVER(app.rename_pop));
    gtk_widget_grab_focus(app.rename_entry);
}

static int row_id_at_y(double y)
{
    GtkListBoxRow *row=gtk_list_box_get_row_at_y(GTK_LIST_BOX(app.list), (int)y);
    if(!row) return -1;
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "class-id"));
}

static void on_list_right(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void)g; (void)n; (void)u;
    int id=row_id_at_y(y);
    if(id>=0) open_rename(id, x, y);
}

static void on_list_long(GtkGestureLongPress *g, double x, double y, gpointer u)
{
    (void)g; (void)u;
    int id=row_id_at_y(y);
    if(id>=0) open_rename(id, x, y);
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer u)
{
    (void)c; (void)keycode; (void)u;
    gboolean ctrl=(state&GDK_CONTROL_MASK)!=0;

    if(ctrl){
        switch(keyval){
            case GDK_KEY_z: do_undo(); return TRUE;
            case GDK_KEY_y: do_redo(); return TRUE;
            case GDK_KEY_s: save_current(); classes_save(); redraw(); return TRUE;
            case GDK_KEY_b:
                gtk_revealer_set_reveal_child(GTK_REVEALER(app.revealer),
                    !gtk_revealer_get_reveal_child(GTK_REVEALER(app.revealer)));
                return TRUE;
            case GDK_KEY_q:
                gtk_window_close(GTK_WINDOW(app.window));
                return TRUE;
        }
        /* Ctrl + 0..9 : reassign class of the selected box */
        if(keyval>=GDK_KEY_0 && keyval<=GDK_KEY_9 && app.selected>=0){
            int id=keyval-GDK_KEY_0;
            if(id<(int)app.classes->len){
                Box *b=&g_array_index(app.boxes, Box, app.selected);
                b->class_id=id;
                push_undo();
                app.dirty=TRUE;
                redraw();
            }
            return TRUE;
        }
        return FALSE;
    }

    switch(keyval){
        case GDK_KEY_a: case GDK_KEY_Left:  go_to(app.cur-1); return TRUE;
        case GDK_KEY_d: case GDK_KEY_Right: go_to(app.cur+1); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal:
            zoom_at(1.25, gtk_widget_get_width(app.area)/2.0,
                          gtk_widget_get_height(app.area)/2.0); return TRUE;
        case GDK_KEY_minus:
            zoom_at(1.0/1.25, gtk_widget_get_width(app.area)/2.0,
                              gtk_widget_get_height(app.area)/2.0); return TRUE;
        case GDK_KEY_f: compute_fit(); redraw(); return TRUE;
        case GDK_KEY_h: app.show_tags=!app.show_tags; redraw(); return TRUE;
        case GDK_KEY_Escape:
            if(app.btn_down){ app.btn_down=FALSE; app.mode=MODE_NONE; redraw(); }
            else app.selected=-1, redraw();
            return TRUE;
        case GDK_KEY_F1: case GDK_KEY_question:
            /* help handled by menu; keep simple here */
            return TRUE;
    }
    if(keyval>=GDK_KEY_0 && keyval<=GDK_KEY_9){
        int id=keyval-GDK_KEY_0;
        if(id<(int)app.classes->len){ app.active_class=id; build_class_list(); redraw(); }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* settings / help / about                                             */
/* ------------------------------------------------------------------ */

static void on_tol_changed(GtkSpinButton *s, gpointer u)
{ (void)u; app.edge_tol=gtk_spin_button_get_value(s); }
static void on_min_changed(GtkSpinButton *s, gpointer u)
{ (void)u; app.min_edge=gtk_spin_button_get_value(s); }
static void on_empty_changed(GtkSwitch *sw, gboolean st, gpointer u)
{ (void)sw; (void)u; app.save_empty=st; }

static void show_settings(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    GtkWidget *win=gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Settings");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app.window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    GtkWidget *grid=gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid),10);
    gtk_grid_set_column_spacing(GTK_GRID(grid),12);
    gtk_widget_set_margin_start(grid,16); gtk_widget_set_margin_end(grid,16);
    gtk_widget_set_margin_top(grid,16);   gtk_widget_set_margin_bottom(grid,16);

    GtkWidget *l1=gtk_label_new("Edge-grab tolerance (screen px)");
    gtk_widget_set_halign(l1,GTK_ALIGN_START);
    GtkWidget *s1=gtk_spin_button_new_with_range(1,40,1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s1), app.edge_tol);
    g_signal_connect(s1,"value-changed",G_CALLBACK(on_tol_changed),NULL);

    GtkWidget *l2=gtk_label_new("Min box edge (image px)");
    gtk_widget_set_halign(l2,GTK_ALIGN_START);
    GtkWidget *s2=gtk_spin_button_new_with_range(1,200,1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s2), app.min_edge);
    g_signal_connect(s2,"value-changed",G_CALLBACK(on_min_changed),NULL);

    GtkWidget *l3=gtk_label_new("Write empty .txt on pass-through");
    gtk_widget_set_halign(l3,GTK_ALIGN_START);
    GtkWidget *s3=gtk_switch_new();
    gtk_widget_set_halign(s3,GTK_ALIGN_START);
    gtk_switch_set_active(GTK_SWITCH(s3), app.save_empty);
    g_signal_connect(s3,"state-set",G_CALLBACK(on_empty_changed),NULL);

    gtk_grid_attach(GTK_GRID(grid),l1,0,0,1,1);
    gtk_grid_attach(GTK_GRID(grid),s1,1,0,1,1);
    gtk_grid_attach(GTK_GRID(grid),l2,0,1,1,1);
    gtk_grid_attach(GTK_GRID(grid),s2,1,1,1,1);
    gtk_grid_attach(GTK_GRID(grid),l3,0,2,1,1);
    gtk_grid_attach(GTK_GRID(grid),s3,1,2,1,1);

    gtk_window_set_child(GTK_WINDOW(win), grid);
    gtk_window_present(GTK_WINDOW(win));
}

static void show_help(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    const char *txt=
        "Mouse\n"
        "  drag empty area: draw new box (active class)\n"
        "  Ctrl+click box: select   Ctrl+drag: move / resize edge\n"
        "  Shift+click box: delete\n"
        "  right-drag: pan    wheel: zoom\n\n"
        "Keyboard\n"
        "  a / d  or  arrows: prev / next image\n"
        "  0-9: active class   Ctrl+0-9: reassign selected box\n"
        "  + / - : zoom    f: fit    h: toggle labels\n"
        "  Ctrl+Z / Ctrl+Y: undo / redo\n"
        "  Ctrl+S: save    Ctrl+B: toggle panel\n"
        "  Esc: cancel current op    Ctrl+Q: quit\n";
    GtkAlertDialog *d=gtk_alert_dialog_new("%s", txt);
    gtk_alert_dialog_set_modal(d, TRUE);
    gtk_alert_dialog_show(d, GTK_WINDOW(app.window));
    g_object_unref(d);
}

static void show_about(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    GtkAlertDialog *d=gtk_alert_dialog_new(
        "Imagnoter\nLightweight native GTK4 YOLO annotator.");
    gtk_alert_dialog_set_modal(d, TRUE);
    gtk_alert_dialog_show(d, GTK_WINDOW(app.window));
    g_object_unref(d);
}

/* ------------------------------------------------------------------ */
/* overlay buttons                                                     */
/* ------------------------------------------------------------------ */

static void ob_zoom_in(GtkButton *b, gpointer u){ (void)b;(void)u;
    zoom_at(1.25, gtk_widget_get_width(app.area)/2.0, gtk_widget_get_height(app.area)/2.0); }
static void ob_zoom_out(GtkButton *b, gpointer u){ (void)b;(void)u;
    zoom_at(1.0/1.25, gtk_widget_get_width(app.area)/2.0, gtk_widget_get_height(app.area)/2.0); }
static void ob_fit(GtkButton *b, gpointer u){ (void)b;(void)u; compute_fit(); redraw(); }
static void ob_edit_toggle(GtkToggleButton *t, gpointer u){ (void)u;
    app.touch_edit=gtk_toggle_button_get_active(t);
    if(app.touch_edit) app.touch_del=FALSE; }
static void ob_del_toggle(GtkToggleButton *t, gpointer u){ (void)u;
    app.touch_del=gtk_toggle_button_get_active(t);
    if(app.touch_del) app.touch_edit=FALSE; }

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

static void report_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg=g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if(app.window){
        GtkAlertDialog *d=gtk_alert_dialog_new("%s", msg);
        gtk_alert_dialog_set_modal(d, TRUE);
        gtk_alert_dialog_show(d, GTK_WINDOW(app.window));
        g_object_unref(d);
    }else{
        g_printerr("%s\n", msg);
    }
    g_free(msg);
}

/* ------------------------------------------------------------------ */
/* title                                                               */
/* ------------------------------------------------------------------ */

static void update_title(void)
{
    if(!app.window) return;
    char buf[256];
    if(app.cur>=0 && app.images && app.cur<(int)app.images->len)
        snprintf(buf,sizeof(buf),"%s%s - Imagnoter",
                 (const char*)g_ptr_array_index(app.images,app.cur),
                 app.dirty?" *":"");
    else
        snprintf(buf,sizeof(buf),"Imagnoter");
    if(strcmp(buf, app.last_title)!=0){
        g_strlcpy(app.last_title, buf, sizeof(app.last_title));
        gtk_window_set_title(GTK_WINDOW(app.window), buf);
    }
}

/* ------------------------------------------------------------------ */
/* config persistence                                                  */
/* ------------------------------------------------------------------ */

static void config_load(void)
{
    char *dir=g_build_filename(g_get_user_config_dir(), "imagnoter", NULL);
    g_mkdir_with_parents(dir, 0755);
    app.config_path=g_build_filename(dir, "imagnoter.ini", NULL);
    g_free(dir);

    GKeyFile *kf=g_key_file_new();
    if(g_key_file_load_from_file(kf, app.config_path, G_KEY_FILE_NONE, NULL)){
        app.edge_tol =g_key_file_get_double (kf,"settings","edge_tol",NULL);
        app.min_edge =g_key_file_get_double (kf,"settings","min_edge",NULL);
        if(g_key_file_has_key(kf,"settings","save_empty",NULL))
            app.save_empty=g_key_file_get_boolean(kf,"settings","save_empty",NULL);
        if(app.edge_tol<=0) app.edge_tol=10;
        if(app.min_edge<=0) app.min_edge=10;
    }
    g_key_file_free(kf);
}

static int saved_w=1100, saved_h=720;
static gboolean saved_panel=TRUE;
static char *saved_folder=NULL;
static char *saved_image=NULL;

static void config_preload_session(void)
{
    GKeyFile *kf=g_key_file_new();
    if(g_key_file_load_from_file(kf, app.config_path, G_KEY_FILE_NONE, NULL)){
        int w=g_key_file_get_integer(kf,"session","win_w",NULL);
        int h=g_key_file_get_integer(kf,"session","win_h",NULL);
        if(w>200) saved_w=w;
        if(h>200) saved_h=h;
        if(g_key_file_has_key(kf,"session","panel",NULL))
            saved_panel=g_key_file_get_boolean(kf,"session","panel",NULL);
        saved_folder=g_key_file_get_string(kf,"session","folder",NULL);
        saved_image =g_key_file_get_string(kf,"session","image",NULL);
    }
    g_key_file_free(kf);
}

static void config_save(void)
{
    GKeyFile *kf=g_key_file_new();
    g_key_file_set_double (kf,"settings","edge_tol", app.edge_tol);
    g_key_file_set_double (kf,"settings","min_edge", app.min_edge);
    g_key_file_set_boolean(kf,"settings","save_empty", app.save_empty);
    if(app.window){
        g_key_file_set_integer(kf,"session","win_w", gtk_widget_get_width(app.window));
        g_key_file_set_integer(kf,"session","win_h", gtk_widget_get_height(app.window));
        g_key_file_set_boolean(kf,"session","panel",
            gtk_revealer_get_reveal_child(GTK_REVEALER(app.revealer)));
    }
    if(app.folder) g_key_file_set_string(kf,"session","folder", app.folder);
    if(app.cur>=0 && app.images && app.cur<(int)app.images->len)
        g_key_file_set_string(kf,"session","image",
                              g_ptr_array_index(app.images, app.cur));
    gsize len=0;
    char *data=g_key_file_to_data(kf, &len, NULL);
    if(data){ g_file_set_contents(app.config_path, data, len, NULL); g_free(data); }
    g_key_file_free(kf);
}

static gboolean on_close(GtkWindow *w, gpointer u)
{
    (void)w; (void)u;
    save_current();
    config_save();
    /* tear down set_parent'd popovers ourselves so no container dispose
       iterates over them */
    if(app.rename_pop){
        gtk_popover_popdown(GTK_POPOVER(app.rename_pop));
        gtk_widget_unparent(app.rename_pop);
        app.rename_pop=NULL;
    }
    if(app.add_pop){
        gtk_popover_popdown(GTK_POPOVER(app.add_pop));
        gtk_widget_unparent(app.add_pop);
        app.add_pop=NULL;
    }
    return FALSE;   /* allow close */
}

/* ------------------------------------------------------------------ */
/* resize -> keep fit if currently at fit                              */
/* ------------------------------------------------------------------ */

static void on_area_resize(GtkDrawingArea *da, int w, int h, gpointer u)
{
    (void)da; (void)w; (void)h; (void)u;
    if(!app.pixbuf) return;
    gboolean was_fit=fabs(app.zoom-app.fit_zoom)<1e-6;
    double old_fit=app.fit_zoom;
    /* recompute fit_zoom for new size */
    int aw=gtk_widget_get_width(app.area), ah=gtk_widget_get_height(app.area);
    if(aw>0 && ah>0){
        double zx=(double)aw/app.img_w, zy=(double)ah/app.img_h;
        app.fit_zoom=zx<zy?zx:zy;
    }
    if(was_fit) compute_fit();
    else (void)old_fit;
    redraw();
}

/* ------------------------------------------------------------------ */
/* build UI                                                            */
/* ------------------------------------------------------------------ */

static GtkWidget *icon_button(const char *icon, const char *tip)
{
    GtkWidget *b=gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

#ifdef IMAGNOTER_SELFTEST
/* used only to exercise the draw/cache/destroy paths in headless CI */
static gboolean selftest_close(gpointer u)
{
    (void)u;
    if(app.window) gtk_window_close(GTK_WINDOW(app.window));
    return G_SOURCE_REMOVE;
}
static gboolean selftest_exercise(gpointer u)
{
    (void)u;
    /* fit (often downscale on big images) -> already drawn once.
       now force zoom-in (crop + upscale rebuild) and a pan (out-of-margin). */
    double cx=gtk_widget_get_width(app.area)/2.0;
    double cy=gtk_widget_get_height(app.area)/2.0;
    zoom_at(2.5, cx, cy);
    app.off_x-=400; app.off_y-=300; redraw();
    zoom_at(2.0, cx, cy);
    app.off_x+=250; redraw();
    g_timeout_add(400, selftest_close, NULL);
    return G_SOURCE_REMOVE;
}
#endif

static void activate(GtkApplication *gapp, gpointer u)
{
    (void)u;
    app.window=gtk_application_window_new(gapp);
    gtk_window_set_default_size(GTK_WINDOW(app.window), saved_w, saved_h);
	GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".modedit:checked { background-image:none; background-color:#3584e4; color:#ffffff; }"
        ".moddel:checked  { background-image:none; background-color:#e01b24; color:#ffffff; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    update_title();

    /* header bar */
    GtkWidget *hb=gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(app.window), hb);

    app.panel_btn=icon_button("sidebar-show-symbolic","Toggle panel (Ctrl+B)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), app.panel_btn);
    GtkWidget *open_btn=gtk_button_new_with_label("Open folder");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), open_btn);
    app.folder_label=gtk_label_new("(no folder)");
    gtk_widget_add_css_class(app.folder_label,"dim-label");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), app.folder_label);

    /* menu button with a small popover */
    GtkWidget *menu_btn=gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn),"open-menu-symbolic");
    GtkWidget *mpop=gtk_popover_new();
    GtkWidget *mbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    gtk_widget_set_margin_start(mbox,6); gtk_widget_set_margin_end(mbox,6);
    gtk_widget_set_margin_top(mbox,6);   gtk_widget_set_margin_bottom(mbox,6);
    GtkWidget *mset=gtk_button_new_with_label("Settings");
    GtkWidget *mhelp=gtk_button_new_with_label("Shortcuts");
    GtkWidget *mabout=gtk_button_new_with_label("About");
    gtk_widget_add_css_class(mset,"flat");
    gtk_widget_add_css_class(mhelp,"flat");
    gtk_widget_add_css_class(mabout,"flat");
    gtk_box_append(GTK_BOX(mbox),mset);
    gtk_box_append(GTK_BOX(mbox),mhelp);
    gtk_box_append(GTK_BOX(mbox),mabout);
    gtk_popover_set_child(GTK_POPOVER(mpop), mbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_btn), mpop);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), menu_btn);

    GtkWidget *redo_btn=icon_button("edit-redo-symbolic","Redo (Ctrl+Y)");
    GtkWidget *undo_btn=icon_button("edit-undo-symbolic","Undo (Ctrl+Z)");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), redo_btn);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), undo_btn);

    g_signal_connect(open_btn,"clicked",G_CALLBACK(on_open_clicked),NULL);
    g_signal_connect(undo_btn,"clicked",G_CALLBACK(do_undo),NULL);
    g_signal_connect(redo_btn,"clicked",G_CALLBACK(do_redo),NULL);
    g_signal_connect(mset,"clicked",G_CALLBACK(show_settings),NULL);
    g_signal_connect(mhelp,"clicked",G_CALLBACK(show_help),NULL);
    g_signal_connect(mabout,"clicked",G_CALLBACK(show_about),NULL);

    /* main horizontal layout */
    GtkWidget *hbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);

    /* left panel */
    app.revealer=gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app.revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    GtkWidget *panel=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    app.panel=panel;
    gtk_widget_set_size_request(panel,200,-1);
    GtkWidget *scroller=gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller,TRUE);
    app.list=gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app.list);
    GtkWidget *addb=gtk_button_new_with_label("+ Add class");
    gtk_widget_set_margin_start(addb,6); gtk_widget_set_margin_end(addb,6);
    gtk_widget_set_margin_top(addb,6);   gtk_widget_set_margin_bottom(addb,6);
    gtk_box_append(GTK_BOX(panel), scroller);
    gtk_box_append(GTK_BOX(panel), addb);
    gtk_revealer_set_child(GTK_REVEALER(app.revealer), panel);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app.revealer), saved_panel);
    gtk_box_append(GTK_BOX(hbox), app.revealer);

    g_signal_connect(app.list,"row-activated",G_CALLBACK(on_row_activated),NULL);
    g_signal_connect(addb,"clicked",G_CALLBACK(on_add_clicked),NULL);

    /* add-class popover */
    app.add_pop=gtk_popover_new();
    gtk_widget_set_parent(app.add_pop, addb);
    app.add_entry=gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.add_entry),"class name");
    gtk_popover_set_child(GTK_POPOVER(app.add_pop), app.add_entry);
    g_signal_connect(app.add_entry,"activate",G_CALLBACK(on_add_commit),NULL);

    /* rename popover (parented to the panel box, NOT the list: a GtkListBox
       routes child removal through gtk_list_box_remove and would loop on a
       non-row child at dispose; a GtkBox just unparents cleanly) */
    app.rename_pop=gtk_popover_new();
    app.rename_entry=gtk_entry_new();
    gtk_popover_set_child(GTK_POPOVER(app.rename_pop), app.rename_entry);
    gtk_widget_set_parent(app.rename_pop, app.panel);
    g_signal_connect(app.rename_entry,"activate",G_CALLBACK(on_rename_commit),NULL);
    app.rename_target=-1;

    /* right-click / long-press on class list -> rename */
    GtkGesture *lr=gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lr), GDK_BUTTON_SECONDARY);
    g_signal_connect(lr,"pressed",G_CALLBACK(on_list_right),NULL);
    gtk_widget_add_controller(app.list, GTK_EVENT_CONTROLLER(lr));
    GtkGesture *ll=gtk_gesture_long_press_new();
    gtk_gesture_long_press_set_delay_factor(GTK_GESTURE_LONG_PRESS(ll),1.0);
    g_signal_connect(ll,"pressed",G_CALLBACK(on_list_long),NULL);
    gtk_widget_add_controller(app.list, GTK_EVENT_CONTROLLER(ll));

    /* canvas + overlay */
    GtkWidget *overlay=gtk_overlay_new();
    gtk_widget_set_hexpand(overlay,TRUE);
    gtk_widget_set_vexpand(overlay,TRUE);
    app.area=gtk_drawing_area_new();
    gtk_widget_set_hexpand(app.area,TRUE);
    gtk_widget_set_vexpand(app.area,TRUE);
    gtk_widget_set_focusable(app.area,TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app.area), on_draw, NULL, NULL);
    g_signal_connect(app.area,"resize",G_CALLBACK(on_area_resize),NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), app.area);

    /* zoom overlay buttons (bottom-right) */
    GtkWidget *zoombox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_widget_set_halign(zoombox,GTK_ALIGN_END);
    gtk_widget_set_valign(zoombox,GTK_ALIGN_END);
    gtk_widget_set_margin_end(zoombox,12);
    gtk_widget_set_margin_bottom(zoombox,12+(int)STATUS_H);
    GtkWidget *zin=icon_button("zoom-in-symbolic","Zoom in");
    GtkWidget *zfit=icon_button("zoom-fit-best-symbolic","Fit");
    GtkWidget *zout=icon_button("zoom-out-symbolic","Zoom out");
    gtk_box_append(GTK_BOX(zoombox),zout);
    gtk_box_append(GTK_BOX(zoombox),zfit);
    gtk_box_append(GTK_BOX(zoombox),zin);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), zoombox);
    g_signal_connect(zin,"clicked",G_CALLBACK(ob_zoom_in),NULL);
    g_signal_connect(zout,"clicked",G_CALLBACK(ob_zoom_out),NULL);
    g_signal_connect(zfit,"clicked",G_CALLBACK(ob_fit),NULL);

    /* touch modifier buttons (bottom-left) */
    GtkWidget *modbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_widget_set_halign(modbox,GTK_ALIGN_START);
    gtk_widget_set_valign(modbox,GTK_ALIGN_END);
    gtk_widget_set_margin_start(modbox,12);
    gtk_widget_set_margin_bottom(modbox,12+(int)STATUS_H);
    GtkWidget *editt=gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(editt),"document-edit-symbolic");
    gtk_widget_set_tooltip_text(editt,"Edit mode (touch): move / resize");
    GtkWidget *delt=gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(delt),"user-trash-symbolic");
    gtk_widget_set_tooltip_text(delt,"Delete mode (touch)");
    gtk_widget_add_css_class(editt, "modedit");
    gtk_widget_add_css_class(delt,  "moddel");
    gtk_box_append(GTK_BOX(modbox),editt);
    gtk_box_append(GTK_BOX(modbox),delt);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), modbox);
    g_signal_connect(editt,"toggled",G_CALLBACK(ob_edit_toggle),NULL);
    g_signal_connect(delt,"toggled",G_CALLBACK(ob_del_toggle),NULL);

    gtk_box_append(GTK_BOX(hbox), overlay);
    gtk_window_set_child(GTK_WINDOW(app.window), hbox);

    /* canvas controllers */
    GtkGesture *click=gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);   /* any button */
    g_signal_connect(click,"pressed",G_CALLBACK(on_pressed),NULL);
    g_signal_connect(click,"released",G_CALLBACK(on_released),NULL);
    gtk_widget_add_controller(app.area, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion=gtk_event_controller_motion_new();
    g_signal_connect(motion,"motion",G_CALLBACK(on_motion),NULL);
    gtk_widget_add_controller(app.area, motion);

    GtkEventController *hover=gtk_event_controller_motion_new();
    g_signal_connect(hover,"motion",G_CALLBACK(on_hover),NULL);
    gtk_widget_add_controller(app.area, hover);

    GtkEventController *scroll=gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll,"scroll",G_CALLBACK(on_scroll),NULL);
    gtk_widget_add_controller(app.area, scroll);

    GtkGesture *zoom=gtk_gesture_zoom_new();
    g_signal_connect(zoom,"begin",G_CALLBACK(on_pinch_begin),NULL);
    g_signal_connect(zoom,"scale-changed",G_CALLBACK(on_pinch_scale),NULL);
    gtk_widget_add_controller(app.area, GTK_EVENT_CONTROLLER(zoom));

    GtkEventController *key=gtk_event_controller_key_new();
    g_signal_connect(key,"key-pressed",G_CALLBACK(on_key),NULL);
    gtk_widget_add_controller(app.window, key);

    g_signal_connect(app.window,"close-request",G_CALLBACK(on_close),NULL);

    /* panel toggle button */
    g_signal_connect(app.panel_btn,"clicked",G_CALLBACK(on_panel_toggle),NULL);

    build_class_list();

    /* restore previous session folder if still present */
    if(saved_folder && g_file_test(saved_folder, G_FILE_TEST_IS_DIR))
        open_folder(saved_folder, saved_image);

    gtk_window_present(GTK_WINDOW(app.window));

#ifdef IMAGNOTER_SELFTEST
    g_timeout_add(1200, selftest_exercise, NULL);
#endif
}

/* panel toggle needs a real callback (revealer toggle) */
static void on_panel_toggle(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    gtk_revealer_set_reveal_child(GTK_REVEALER(app.revealer),
        !gtk_revealer_get_reveal_child(GTK_REVEALER(app.revealer)));
}

int main(int argc, char **argv)
{
    memset(&app, 0, sizeof(app));
    app.cur=-1;
    app.selected=-1;
    app.active_class=-1;
    app.show_tags=TRUE;
    app.zoom=1.0; app.fit_zoom=1.0;
    app.edge_tol=10;
    app.min_edge=10;
    app.save_empty=TRUE;
    app.images=g_ptr_array_new_with_free_func(g_free);
    app.classes=g_ptr_array_new_with_free_func(g_free);
    app.boxes=g_array_new(FALSE, FALSE, sizeof(Box));
    app.history=g_ptr_array_new();
    app.hist_idx=-1;

    config_load();
    config_preload_session();

    app.app=gtk_application_new("org.imagnoter.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.app,"activate",G_CALLBACK(activate),NULL);

    /* connect the real panel toggle after activate created the widgets:
       handled inside activate via on_panel_toggle */
    (void)on_panel_toggle;

    int status=g_application_run(G_APPLICATION(app.app), argc, argv);
    g_object_unref(app.app);
    return status;
}
