/*
 * Headless VteTerminal renderer used to check inline image support:
 * feeds a file to a terminal of the given size, snapshots the widget to a
 * PNG, dumps the visible text next to it (<out.png>.txt), and writes
 * everything the terminal would send to the child (kitty graphics replies,
 * DA responses, ...) to a file.
 *
 * Build against an uninstalled build:
 *   PKG_CONFIG_PATH=build/meson-uninstalled \
 *     gcc -o render-test tools/render-test.c $(pkg-config --cflags --libs vte-2.91-gtk4)
 * Run (needs a display; GDK_BACKEND=x11 DISPLAY=:0 works under Xwayland):
 *   LD_LIBRARY_PATH=build/src ./render-test input.txt out.png replies.out 80 24
 */
#include <vte/vte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean running = TRUE;
static gboolean pump_done(gpointer d) { *(gboolean*)d = TRUE; return G_SOURCE_REMOVE; }
static void pump(int ms) { gboolean done = FALSE; g_timeout_add(ms, pump_done, &done); while (!done) g_main_context_iteration(NULL, TRUE); }
static const char *out_png;
static const char *out_commit;
static FILE *commit_fp;
static int cols = 80, rows = 24;

static void on_commit(VteTerminal *t, const char *text, guint size, gpointer d) {
  if (commit_fp) { fwrite(text, 1, size, commit_fp); fflush(commit_fp); }
}

static gboolean snap(gpointer data) {
  GtkWidget *term = data;
  int w = gtk_widget_get_width(term), h = gtk_widget_get_height(term);
  GdkPaintable *p = gtk_widget_paintable_new(term);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(p, snapshot, w, h);
  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surf);
  if (node) { gsk_render_node_draw(node, cr); gsk_render_node_unref(node); }
  cairo_destroy(cr);
  cairo_surface_write_to_png(surf, out_png);
  { char *txt = vte_terminal_get_text_format(VTE_TERMINAL(term), VTE_FORMAT_TEXT);
    char *tp = g_strconcat(out_png, ".txt", NULL);
    g_file_set_contents(tp, txt ? txt : "", -1, NULL); g_free(txt); g_free(tp); }
  cairo_surface_destroy(surf);
  g_object_unref(p);
  fprintf(stderr, "snapshot %dx%d -> %s (cell %ldx%ld)\n", w, h, out_png,
          vte_terminal_get_char_width(VTE_TERMINAL(term)), vte_terminal_get_char_height(VTE_TERMINAL(term)));
  running = FALSE;
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s input out.png [commit.out] [cols rows]\n", argv[0]); return 2; }
  const char *in_path = argv[1]; out_png = argv[2];
  if (argc > 3) { out_commit = argv[3]; commit_fp = fopen(out_commit, "wb"); }
  if (argc > 5) { cols = atoi(argv[4]); rows = atoi(argv[5]); }
  gtk_init();
  GtkWidget *win = gtk_window_new();
  GtkWidget *term = vte_terminal_new();
  PangoFontDescription *fd = pango_font_description_from_string("Noto Sans Mono 12");
  vte_terminal_set_font(VTE_TERMINAL(term), fd);
  pango_font_description_free(fd);
  vte_terminal_set_size(VTE_TERMINAL(term), cols, rows);
  g_object_set(term, "enable-sixel", TRUE, NULL);
  g_signal_connect(term, "commit", G_CALLBACK(on_commit), NULL);
  GtkWidget *fixed = gtk_fixed_new();
  gtk_fixed_put(GTK_FIXED(fixed), term, 0, 0);
  gtk_window_set_child(GTK_WINDOW(win), fixed);
  gtk_window_present(GTK_WINDOW(win));
  gchar *data = NULL; gsize len = 0;
  if (!g_file_get_contents(in_path, &data, &len, NULL)) { fprintf(stderr, "cannot read %s\n", in_path); return 1; }
  /* let the widget realize, then size it so that the terminal has the wanted grid */
  pump(150);
  {
    VteTerminal *t = VTE_TERMINAL(term);
    int cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    int w = cols * cw, h = rows * ch;
    gtk_widget_set_size_request(term, w, h);
    pump(300);
    long gc = vte_terminal_get_column_count(t), gr = vte_terminal_get_row_count(t);
    if (gc != cols || gr != rows) {
      w += (cols - gc) * cw; h += (rows - gr) * ch;
      gtk_widget_set_size_request(term, w, h);
      pump(300);
    }
    fprintf(stderr, "grid %ldx%ld\n", vte_terminal_get_column_count(t), vte_terminal_get_row_count(t));
  }
  vte_terminal_feed(VTE_TERMINAL(term), data, len);
  g_timeout_add(400, snap, term);
  while (running) g_main_context_iteration(NULL, TRUE);
  if (commit_fp) fclose(commit_fp);
  return 0;
}
