/* =============================================================
   EduWeek – school week calendar (Emery / Pebble Time 2 only)

   List window   : one row per week, starting at the running week
                     WK 39          21/09-25/09
                     P1                    WK 4
   Detail window : mockup layout, UP/DOWN = previous/next week

   Buttons and touch: plain click handlers only. With
   app_touch_navigation_enable(true) the system touch bridge turns
   gestures into those same clicks:
     MenuLayer : drag to scroll, tap a row to open it
     Detail    : swipe down = UP (previous), swipe up = DOWN (next),
                 swipe right = BACK
   ============================================================= */
#include <pebble.h>

/* ----------------------------------------------------------
   Configuration
---------------------------------------------------------- */
/* Mockup purple is #643994. Nearest palette colour is GColorLiberty
   (bluish); GColorIndigo reads as purple on the Time 2 display. */
#define ACCENT_COLOR     GColorImperialPurple
#define SEPARATOR_COLOR  GColorImperialPurple

#define MAX_WEEKS        60      /* must match MAX_WEEKS in index.js */
#define PAYLOAD_MAX      3072
#define INBOX_SIZE       4096
#define OUTBOX_SIZE      64

#define PERSIST_KEY_LEN     1
#define PERSIST_KEY_CHUNK0  10
#define PERSIST_CHUNK       240   /* < PERSIST_DATA_MAX_LENGTH (256) */

/* Layout (Emery 200 x 228) */
#define SIDE_PAD         10
#define LIST_PAD         5       /* "WK 39" + "21/09-25/09" need ~180 px */
#define LIST_ROW_H       56
#define LIST_LINE_H      26
#define REPEAT_MS        250

/* Custom fonts render with some space above the glyphs. If text looks
   too low / high inside its band after the first run, tune these. */
#define NUDGE_SMALL      2
#define NUDGE_LARGE      3

/* ----------------------------------------------------------
   Data
---------------------------------------------------------- */
typedef struct {
  int32_t monday;        /* days since 1970-01-01 */
  uint8_t iso_week;
  char    period[6];
  char    edu[16];
  char    info[32];
} WeekItem;

static WeekItem s_weeks[MAX_WEEKS];
static int      s_count;
static char     s_payload[PAYLOAD_MAX];
static char     s_status[48] = "Loading weeks...";

static GFont s_font_small;   /* Segoe UI Bold 20 */
static GFont s_font_large;   /* Segoe UI Bold 26 */

static Window    *s_list_window;
static MenuLayer *s_menu_layer;
static Window    *s_detail_window;
static Layer     *s_detail_layer;
static int        s_detail_index;

static const char *const MONTHS[12] = {
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December"
};
static const char *const MONTHS_SHORT[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const DAY_NAMES[5] = { "MO", "TU", "WE", "TH", "FR" };

/* ----------------------------------------------------------
   Date helpers (proleptic Gregorian, no time zones involved)
---------------------------------------------------------- */
static int32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int32_t  era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int *y, int *m, int *d) {
  z += 719468;
  const int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp  = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)((int32_t)yoe + era * 400) + (*m <= 2);
}

static int32_t today_days(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  return days_from_civil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
}

/* "21/09-25/09" */
static void format_range(const WeekItem *w, char *buf, size_t size) {
  int y1, m1, d1, y2, m2, d2;
  civil_from_days(w->monday, &y1, &m1, &d1);
  civil_from_days(w->monday + 4, &y2, &m2, &d2);
  snprintf(buf, size, "%02d/%02d-%02d/%02d", d1, m1, d2, m2);
}

/* ----------------------------------------------------------
   Payload parsing:  monday|week|period|edu|info\n...
---------------------------------------------------------- */
static void copy_field(char *dst, size_t size, const char *start, const char *end) {
  size_t n = (size_t)(end - start);
  if (n >= size) {
    n = size - 1;
    /* never cut a UTF-8 character in half */
    while (n > 0 && ((uint8_t)start[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, start, n);
  dst[n] = '\0';
}

static void parse_payload(const char *p) {
  const int32_t today = today_days();
  s_count = 0;

  while (*p && s_count < MAX_WEEKS) {
    const char *eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);

    /* locate the 4 separators on this line */
    const char *f[5] = { p };
    int n = 1;
    for (const char *c = p; c < eol && n < 5; c++) {
      if (*c == '|') f[n++] = c + 1;
    }

    if (n == 5) {
      const int32_t monday = atoi(f[0]);
      if (monday + 7 > today) {            /* drop weeks that are over */
        WeekItem *w = &s_weeks[s_count++];
        w->monday   = monday;
        w->iso_week = (uint8_t)atoi(f[1]);
        copy_field(w->period, sizeof(w->period), f[2], f[3] - 1);
        copy_field(w->edu,    sizeof(w->edu),    f[3], f[4] - 1);
        copy_field(w->info,   sizeof(w->info),   f[4], eol);
      }
    }
    p = *eol ? eol + 1 : eol;
  }
}

/* ----------------------------------------------------------
   Persistence: payload split over 240-byte keys
---------------------------------------------------------- */
static void save_payload(void) {
  const int len = (int)strlen(s_payload);
  persist_write_int(PERSIST_KEY_LEN, len);
  for (int off = 0, k = 0; off < len; off += PERSIST_CHUNK, k++) {
    const int n = (len - off) < PERSIST_CHUNK ? (len - off) : PERSIST_CHUNK;
    persist_write_data(PERSIST_KEY_CHUNK0 + k, s_payload + off, n);
  }
}

static void load_payload(void) {
  if (!persist_exists(PERSIST_KEY_LEN)) return;
  const int len = persist_read_int(PERSIST_KEY_LEN);
  if (len <= 0 || len >= PAYLOAD_MAX) return;

  for (int off = 0, k = 0; off < len; off += PERSIST_CHUNK, k++) {
    const int n = (len - off) < PERSIST_CHUNK ? (len - off) : PERSIST_CHUNK;
    if (persist_read_data(PERSIST_KEY_CHUNK0 + k, s_payload + off, n) != n) return;
  }
  s_payload[len] = '\0';
  parse_payload(s_payload);
}

/* ----------------------------------------------------------
   Drawing helpers
---------------------------------------------------------- */
/* Draw one line of text vertically centred in a band */
static void draw_text_in_band(GContext *ctx, const char *text, GFont font, GRect band,
                              GTextAlignment align) {
  const int nudge = (font == s_font_large) ? NUDGE_LARGE : NUDGE_SMALL;
  const GSize sz  = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, band.size.w, band.size.h * 2),
      GTextOverflowModeTrailingEllipsis, align);
  const GRect r = GRect(band.origin.x,
                        band.origin.y + (band.size.h - sz.h) / 2 - nudge,
                        band.size.w, sz.h + nudge);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, align, NULL);
}

static int16_t text_width(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 1000, 60), GTextOverflowModeWordWrap, GTextAlignmentLeft).w;
}

/* ----------------------------------------------------------
   Detail window
---------------------------------------------------------- */
static void detail_update_proc(Layer *layer, GContext *ctx) {
  if (s_count == 0) return;
  if (s_detail_index >= s_count) s_detail_index = s_count - 1;

  const WeekItem *w = &s_weeks[s_detail_index];
  const GRect b     = layer_get_bounds(layer);
  const int16_t W   = b.size.w;
  const int16_t inner_w = W - 2 * SIDE_PAD;
  char buf[40];

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  /* 1. Header bar: ISO week */
  graphics_context_set_fill_color(ctx, ACCENT_COLOR);
  graphics_fill_rect(ctx, GRect(0, 0, W, 38), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  snprintf(buf, sizeof(buf), "Week %d", w->iso_week);
  draw_text_in_band(ctx, buf, s_font_large, GRect(0, 0, W, 38), GTextAlignmentCenter);

  /* 2. Month (Mmmm); a week spanning two months shows both */
  int y1, m1, d1, y5, m5, d5;
  civil_from_days(w->monday, &y1, &m1, &d1);
  civil_from_days(w->monday + 4, &y5, &m5, &d5);
  if (m1 == m5) {
    snprintf(buf, sizeof(buf), "%s", MONTHS[m1 - 1]);
  } else {
    snprintf(buf, sizeof(buf), "%s / %s", MONTHS[m1 - 1], MONTHS[m5 - 1]);
    if (text_width(buf, s_font_small) > inner_w) {
      snprintf(buf, sizeof(buf), "%s / %s", MONTHS_SHORT[m1 - 1], MONTHS_SHORT[m5 - 1]);
    }
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  draw_text_in_band(ctx, buf, s_font_small, GRect(SIDE_PAD + 4, 40, inner_w - 4, 26),
                    GTextAlignmentLeft);

  /* 3 + 4. Day names, divider, day numbers (today in accent colour) */
  const int16_t col_w  = inner_w / 5;
  const int32_t today  = today_days();
  for (int i = 0; i < 5; i++) {
    const GRect col_top = GRect(SIDE_PAD + i * col_w, 66, col_w, 26);
    const GRect col_bot = GRect(SIDE_PAD + i * col_w, 96, col_w, 26);
    int yy, mm, dd;
    civil_from_days(w->monday + i, &yy, &mm, &dd);
    snprintf(buf, sizeof(buf), "%d", dd);

    graphics_context_set_text_color(ctx, GColorBlack);
    draw_text_in_band(ctx, DAY_NAMES[i], s_font_small, col_top, GTextAlignmentCenter);
    graphics_context_set_text_color(ctx, (w->monday + i == today) ? ACCENT_COLOR : GColorBlack);
    draw_text_in_band(ctx, buf, s_font_small, col_bot, GTextAlignmentCenter);
  }
  graphics_context_set_stroke_color(ctx, ACCENT_COLOR);
  graphics_draw_line(ctx, GPoint(SIDE_PAD, 94), GPoint(W - SIDE_PAD - 1, 94));

  /* 5. Period bar */
  graphics_context_set_fill_color(ctx, ACCENT_COLOR);
  graphics_fill_rect(ctx, GRect(0, 124, W, 30), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  snprintf(buf, sizeof(buf), "Periode %s", w->period);
  draw_text_in_band(ctx, buf, s_font_small, GRect(0, 124, W, 30), GTextAlignmentCenter);

  /* 6. Edu week */
  graphics_context_set_text_color(ctx, ACCENT_COLOR);
  if (w->edu[0] >= '0' && w->edu[0] <= '9') {     /* "11" -> "Week 11", "V" stays "V" */
    snprintf(buf, sizeof(buf), "Week %s", w->edu);
  } else {
    snprintf(buf, sizeof(buf), "%s", w->edu);
  }
  draw_text_in_band(ctx, buf, s_font_large, GRect(SIDE_PAD, 156, inner_w, 32),
                    GTextAlignmentCenter);
  graphics_context_set_stroke_color(ctx, ACCENT_COLOR);
  graphics_draw_line(ctx, GPoint(SIDE_PAD, 190), GPoint(W - SIDE_PAD - 1, 190));

  /* 7. Info: large font, small font if it does not fit */
  if (w->info[0]) {
    const GFont f = (text_width(w->info, s_font_large) <= inner_w) ? s_font_large : s_font_small;
    draw_text_in_band(ctx, w->info, f, GRect(SIDE_PAD, 193, inner_w, 34), GTextAlignmentCenter);
  }
}

static void detail_show(int index) {
  if (index < 0 || index >= s_count) return;
  s_detail_index = index;
  layer_mark_dirty(s_detail_layer);
  /* keep the list in step, so BACK lands on the week last viewed */
  menu_layer_set_selected_index(s_menu_layer, (MenuIndex){ .section = 0, .row = index },
                                MenuRowAlignCenter, false);
}

static void detail_up_click(ClickRecognizerRef rec, void *ctx) {
  detail_show(s_detail_index - 1);
}

static void detail_down_click(ClickRecognizerRef rec, void *ctx) {
  detail_show(s_detail_index + 1);
}

static void detail_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   REPEAT_MS, detail_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, REPEAT_MS, detail_down_click);
}

static void detail_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_detail_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_detail_layer, detail_update_proc);
  layer_add_child(root, s_detail_layer);
}

static void detail_window_unload(Window *window) {
  layer_destroy(s_detail_layer);
  s_detail_layer = NULL;
}

static void detail_push(int index) {
  s_detail_index = index;
  if (!s_detail_window) {
    s_detail_window = window_create();
    window_set_background_color(s_detail_window, GColorWhite);
    window_set_click_config_provider(s_detail_window, detail_click_config);
    window_set_window_handlers(s_detail_window, (WindowHandlers){
      .load   = detail_window_load,
      .unload = detail_window_unload,
    });
  }
  window_stack_push(s_detail_window, true);
}

/* ----------------------------------------------------------
   List window (MenuLayer)
---------------------------------------------------------- */
static uint16_t menu_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_count > 0 ? (uint16_t)s_count : 1;      /* 1 row for the status text */
}

static int16_t menu_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return s_count > 0 ? LIST_ROW_H : layer_get_bounds(menu_layer_get_layer(ml)).size.h;
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  const GRect b = layer_get_bounds(cell);

  if (s_count == 0) {
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, s_status, s_font_small,
                       GRect(SIDE_PAD, b.size.h / 2 - 30, b.size.w - 2 * SIDE_PAD, 60),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  const WeekItem *w = &s_weeks[idx->row];
  const bool hl     = menu_cell_layer_is_highlighted(cell);
  const GRect line1 = GRect(LIST_PAD, 2, b.size.w - 2 * LIST_PAD, LIST_LINE_H);
  const GRect line2 = GRect(LIST_PAD, 2 + LIST_LINE_H, b.size.w - 2 * LIST_PAD, LIST_LINE_H);
  char left[12], right[16], edu[20];

  graphics_context_set_text_color(ctx, hl ? GColorWhite : GColorBlack);

  snprintf(left, sizeof(left), "WK %d", w->iso_week);
  format_range(w, right, sizeof(right));
  draw_text_in_band(ctx, left,   s_font_small, line1, GTextAlignmentLeft);
  if (w->edu[0] >= '0' && w->edu[0] <= '9') {     /* "11" -> "WK 11", "V" stays "V" */
    snprintf(edu, sizeof(edu), "WK %s", w->edu);
  } else {
    snprintf(edu, sizeof(edu), "%s", w->edu);
  }
  draw_text_in_band(ctx, edu,    s_font_small, line1, GTextAlignmentRight);

  snprintf(left, sizeof(left), "P%s", w->period);          // line 371 (unchanged)
  if (w->info[0]) {                        /* "P1 T" : first character of Info */
    size_t len = strlen(left);
    size_t i   = 0;
    left[len++] = ' ';
    do {                                   /* copy a whole UTF-8 character */
      left[len++] = w->info[i++];
    } while (((uint8_t)w->info[i] & 0xC0) == 0x80);
    left[len] = '\0';
  }
  draw_text_in_band(ctx, left,   s_font_small, line2, GTextAlignmentLeft);
  draw_text_in_band(ctx, right,  s_font_small, line2, GTextAlignmentRight);

  if (!hl) {
    graphics_context_set_stroke_color(ctx, SEPARATOR_COLOR);
    graphics_draw_line(ctx, GPoint(LIST_PAD, b.size.h - 1),
                       GPoint(b.size.w - LIST_PAD - 1, b.size.h - 1));
  }
}

static void menu_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (s_count > 0) detail_push(idx->row);
}

static void list_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  s_menu_layer = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows    = menu_num_rows,
    .get_cell_height = menu_cell_height,
    .draw_row        = menu_draw_row,
    .select_click    = menu_select,
  });
  menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu_layer, ACCENT_COLOR, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void list_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

static void refresh_ui(bool jump_to_current) {
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
    if (jump_to_current && s_count > 0) {
      menu_layer_set_selected_index(s_menu_layer, (MenuIndex){ 0, 0 }, MenuRowAlignTop, false);
    }
  }
  if (s_detail_layer) layer_mark_dirty(s_detail_layer);
}

/* ----------------------------------------------------------
   AppMessage
---------------------------------------------------------- */
static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_WEEKS);
  if (t && t->type == TUPLE_CSTRING) {
    strncpy(s_payload, t->value->cstring, PAYLOAD_MAX - 1);
    s_payload[PAYLOAD_MAX - 1] = '\0';
    parse_payload(s_payload);
    save_payload();
    const bool detail_open = window_stack_get_top_window() == s_detail_window;
    refresh_ui(!detail_open);
    return;
  }

  t = dict_find(iter, MESSAGE_KEY_STATUS);
  if (t && t->type == TUPLE_CSTRING) {
    strncpy(s_status, t->value->cstring, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    APP_LOG(APP_LOG_LEVEL_INFO, "Status: %s", s_status);
    if (s_count == 0) refresh_ui(false);   /* cached weeks stay visible */
  }
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
}

/* ----------------------------------------------------------
   App lifecycle
---------------------------------------------------------- */
static void init(void) {
  s_font_small = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_20));
  s_font_large = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_24));

  load_payload();                          /* show cached weeks right away */

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);

#if defined(PBL_TOUCH)
  app_touch_navigation_enable(true);       /* touch -> click bridge */
#endif

  s_list_window = window_create();
  window_set_window_handlers(s_list_window, (WindowHandlers){
    .load   = list_window_load,
    .unload = list_window_unload,
  });
  window_stack_push(s_list_window, true);
}

static void deinit(void) {
  if (s_detail_window) window_destroy(s_detail_window);
  window_destroy(s_list_window);
  fonts_unload_custom_font(s_font_small);
  fonts_unload_custom_font(s_font_large);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
